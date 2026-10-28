#!/usr/bin/env python3
"""Fit conservative stereo and residual body corrections to frozen PianoCraft data."""

from __future__ import annotations

import copy
import hashlib
import json
import math
import subprocess
import wave
import warnings
from pathlib import Path

import numpy as np
from scipy import __version__ as scipy_version
from scipy.io import wavfile
from scipy.io.wavfile import WavFileWarning
from scipy.ndimage import uniform_filter1d
from scipy.optimize import least_squares


ROOT = Path(__file__).resolve().parents[1]
AUDIO_ROOT = ROOT / "calibration" / "meta-piano"
RENDER_ROOT = ROOT / "calibration" / "model-renders"
RENDERER = ROOT / "build" / "pianocraft-render"
ARCHIVE = ROOT / "calibration" / "meta-open-sound-meta-piano-wav.zip"
PROVENANCE = ROOT / "model" / "provenance.json"
HEADER = ROOT / "src" / "model" / "CalibrationData.hpp"
REPORT = ROOT / "model" / "calibration-report.json"
ALGORITHM_VERSION = 5
MIN_MIDI = 21
MAX_MIDI = 108
PERMITTED_AUDIO_LAYERS = (1, 3, 4)
VELOCITY_ANCHORS = {1: 40.0 / 127.0, 3: 80.0 / 127.0, 4: 120.0 / 127.0}
REGISTERS = {"bass": (21, 47), "middle": (48, 71), "treble": (72, 108)}
REGION_INDEX = {"bass": 0, "middle": 1, "treble": 2}
BODY_FITTING_KEYS = {
    "bass": [23, 35, 47],
    "middle": [48, 60, 71],
    "treble": [72, 84, 108],
}
BODY_MODE_FREQUENCIES = np.array([72.0, 103.0, 149.0, 213.0, 307.0, 441.0, 635.0, 914.0, 1316.0, 1895.0, 2728.0, 3928.0, 5656.0, 8144.0])
FROZEN_PARTIAL_DIGEST = "ffb3c4dfa99510538f210d649e9d919f747a358dfa29df9549dcd9f071f0dfd3"
FROZEN_VELOCITY = {
    "bass": (1.2703665364, 0.0, 1.0),
    "middle": (1.6274561736, 0.0, 1.0),
    "treble": (2.1954300443, 0.0, 1.0),
}
LEGACY_FIELDS = ("fundamental", "inharmonicity", "decaySeconds", "loudness", "hammerStiffness")
VELOCITY_FIELDS = ("velocityGainExponent", "velocityBrightnessExponent", "hammerStiffnessScale")
STEREO_FIELDS = ("stereoLeftGain", "stereoRightGain")
DIRECT_BOUNDS = (0.5, 1.5)
RESONANCE_BOUNDS = (0.0, 2.0)
TILT_BOUNDS = (-1.0, 1.0)
STEREO_BOUNDS = (0.5, 1.5)


def note_name(midi: int) -> str:
    names = ("C", "Cs", "D", "Ds", "E", "F", "Fs", "G", "Gs", "A", "As", "B")
    return f"{names[midi % 12]}{midi // 12 - 1}"


def audio_path(midi: int, velocity: int) -> Path:
    return AUDIO_ROOT / f"INSPiano_Vel{velocity:02d}_{note_name(midi)}_01.wav"


def register_name(midi: int) -> str:
    return next(name for name, (lower, upper) in REGISTERS.items() if lower <= midi <= upper)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def partial_digest(keys: list[dict]) -> str:
    payload = [[key["midi"], key["partials"]] for key in keys]
    return hashlib.sha256(json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def verify_corpus(provenance: dict) -> dict:
    expected = {audio_path(midi, velocity).name for midi in range(MIN_MIDI, MAX_MIDI + 1) for velocity in range(1, 5)}
    actual = {path.name for path in AUDIO_ROOT.glob("*.wav")}
    if actual != expected or len(actual) != 352:
        raise RuntimeError("The expected 352-WAV inventory is not present")
    for velocity in PERMITTED_AUDIO_LAYERS:
        for midi in range(MIN_MIDI, MAX_MIDI + 1):
            with wave.open(str(audio_path(midi, velocity)), "rb") as stream:
                if (stream.getframerate(), stream.getnchannels(), stream.getsampwidth(), stream.getcomptype()) != (48000, 2, 3, "NONE"):
                    raise RuntimeError(f"Unexpected WAV format: {stream.getparams()}")
    source = provenance["sources"][0]
    archive_present = ARCHIVE.is_file()
    archive_hash = sha256(ARCHIVE) if archive_present else None
    if archive_present and archive_hash != source["sha256"]:
        raise RuntimeError("Source archive SHA256 mismatch")
    return {
        "id": source["id"], "url": source["url"], "license": source["license"], "archive": source["archive"],
        "archivePresent": archive_present, "archiveSha256Verified": archive_present,
        "sourceArchiveSha256": source["sha256"], "actualArchiveSha256": archive_hash,
        "wavCount": len(actual), "wavFormat": "stereo signed 24-bit PCM, 48000 Hz",
        "audioLayersOpened": list(PERMITTED_AUDIO_LAYERS), "layer2AudioRead": False,
    }


def load_frozen_report() -> dict:
    if not REPORT.is_file():
        raise RuntimeError("Frozen calibration report required; modal and velocity refitting are disabled")
    frozen = json.loads(REPORT.read_text())
    if len(frozen.get("keys", [])) != 88 or partial_digest(frozen["keys"]) != FROZEN_PARTIAL_DIGEST:
        raise RuntimeError("Frozen per-partial digest mismatch")
    quality = frozen.get("qualitySummary", {})
    if quality.get("secondaryCandidateCount") != 106 or quality.get("secondaryAcceptedCount") != 0:
        raise RuntimeError("The frozen 0/106 selective-unison result is missing")
    for name, expected in FROZEN_VELOCITY.items():
        fit = frozen.get("registerExcitationFits", {}).get(name, {})
        actual = tuple(fit.get(field) for field in VELOCITY_FIELDS)
        if actual != expected:
            raise RuntimeError(f"Frozen velocity parameters changed in {name}: {actual}")
    if frozen.get("finalLayer2Validation", {}).get("status") != "pending_untouched":
        raise RuntimeError("Layer-2 final validation must remain pending")
    return frozen


def read_stereo(path: Path, permitted_layer: int | None = None) -> tuple[np.ndarray, int]:
    if permitted_layer is not None and permitted_layer not in PERMITTED_AUDIO_LAYERS:
        raise RuntimeError("Layer 2 is forbidden in this calibration run")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", WavFileWarning)
        sample_rate, samples = wavfile.read(path)
    if sample_rate != 48000 or samples.ndim != 2 or samples.shape[1] != 2:
        raise RuntimeError(f"Unsupported audio: {path}")
    return samples.astype(np.float64) / float(np.iinfo(samples.dtype).max), sample_rate


def find_onset(mono: np.ndarray, sample_rate: int) -> int:
    width = max(1, int(0.004 * sample_rate))
    rms = np.sqrt(np.maximum(uniform_filter1d(mono * mono, width), 0.0))
    candidates = np.flatnonzero(rms >= max(float(rms.max()) * 0.002, 1.0e-8))
    return int(candidates[0]) if candidates.size else 0


def sustained_stereo_feature(midi: int, velocity: int) -> dict:
    stereo, sample_rate = read_stereo(audio_path(midi, velocity), velocity)
    onset = find_onset(stereo.mean(axis=1), sample_rate)
    treble = midi >= 72
    start = onset + int((0.03 if treble else 0.15) * sample_rate)
    stop = min(len(stereo), start + int((0.30 if treble else 0.50) * sample_rate))
    if stop - start < int(0.1 * sample_rate):
        return {"supported": False, "leftRms": None, "rightRms": None}
    window = stereo[start:stop]
    left = float(np.sqrt(np.mean(window[:, 0] ** 2)))
    right = float(np.sqrt(np.mean(window[:, 1] ** 2)))
    return {
        "supported": left > 1.0e-10 and right > 1.0e-10,
        "leftRms": left,
        "rightRms": right,
        "windowSeconds": [start / sample_rate, stop / sample_rate],
    }


def model_pan_gains(midi: int) -> tuple[float, float]:
    pan = float(np.clip((midi - 64.5) / 55.0, -0.55, 0.55))
    return math.sqrt(0.5 * (1.0 - pan)), math.sqrt(0.5 * (1.0 + pan))


def stereo_balance(midi: int, feature: dict) -> float | None:
    if not feature["supported"]:
        return None
    model_left, model_right = model_pan_gains(midi)
    return math.log(feature["leftRms"] / model_left) - math.log(feature["rightRms"] / model_right)


def fit_stereo(fitting_midis: list[int], features: dict[int, dict[int, dict]]) -> tuple[dict[int, tuple[float, float]], dict]:
    raw = {}
    for midi in fitting_midis:
        balances = [stereo_balance(midi, features[midi][velocity]) for velocity in PERMITTED_AUDIO_LAYERS]
        balances = [value for value in balances if value is not None]
        if balances:
            raw[midi] = float(np.median(balances))
    values = {}
    diagnostics = {}
    shrinkage = 0.35
    for name, (lower, upper) in REGISTERS.items():
        register_midis = [midi for midi in fitting_midis if lower <= midi <= upper]
        supported = [midi for midi in register_midis if midi in raw]
        smoothed = {}
        for midi in register_midis:
            neighbors = [key for key in supported if abs(key - midi) <= 2]
            if not neighbors:
                continue
            distances = np.array([key - midi for key in neighbors], dtype=np.float64)
            weights = np.exp(-0.5 * (distances / 1.25) ** 2)
            smoothed[midi] = float(np.sum(weights * np.array([raw[key] for key in neighbors])) / weights.sum())
        clamped = []
        for midi in register_midis:
            if midi not in smoothed:
                values[midi] = (1.0, 1.0)
                continue
            left = math.exp(0.5 * shrinkage * smoothed[midi])
            right = math.exp(-0.5 * shrinkage * smoothed[midi])
            bounded = (float(np.clip(left, *STEREO_BOUNDS)), float(np.clip(right, *STEREO_BOUNDS)))
            if bounded != (left, right):
                clamped.append(midi)
            values[midi] = bounded
        diagnostics[name] = {
            "supportedFittingKeys": supported,
            "unsupportedFittingKeys": [midi for midi in register_midis if midi not in smoothed],
            "clampedKeys": clamped,
            "rawBalanceMedian": float(np.median([raw[midi] for midi in supported])) if supported else None,
            "smoothedBalanceRange": [min(smoothed.values()), max(smoothed.values())] if smoothed else None,
            "shrinkage": shrinkage,
            "smoothing": "Gaussian over fitting keys within two semitones inside each register; median across layers 1,3,4",
        }
    return values, diagnostics


def interpolate_stereo(midi: int, lower: dict, upper: dict) -> tuple[float, float]:
    fraction = (midi - lower["midi"]) / (upper["midi"] - lower["midi"])
    return tuple(
        math.exp((1.0 - fraction) * math.log(lower["values"][field]) + fraction * math.log(upper["values"][field]))
        for field in STEREO_FIELDS
    )


def stereo_development(midi: int, layer_features: dict[int, dict], gains: tuple[float, float]) -> dict:
    model_left, model_right = model_pan_gains(midi)
    baseline_ratio = math.log(model_left / model_right)
    corrected_ratio = baseline_ratio + math.log(gains[0] / gains[1])
    layers = {}
    for velocity, feature in layer_features.items():
        measured = math.log(feature["leftRms"] / feature["rightRms"]) if feature["supported"] else None
        layers[str(velocity)] = {
            **feature,
            "measuredLogLeftRightRatio": measured,
            "baselineAbsoluteLogError": abs(measured - baseline_ratio) if measured is not None else None,
            "correctedAbsoluteLogError": abs(measured - corrected_ratio) if measured is not None else None,
        }
    return {"developmentOnly": True, "usedForCalibration": False, "layers": layers}


def percentile_summary(values: list[float]) -> dict:
    if not values:
        return {"count": 0, "median": None, "p95": None}
    array = np.asarray(values, dtype=np.float64)
    return {"count": int(array.size), "median": float(np.median(array)), "p95": float(np.percentile(array, 95))}


def stereo_metrics(development: dict[int, dict]) -> dict:
    def summarize(midis: list[int]) -> dict:
        baseline, corrected = [], []
        by_layer = {}
        for velocity in PERMITTED_AUDIO_LAYERS:
            base = [development[midi]["layers"][str(velocity)]["baselineAbsoluteLogError"] for midi in midis]
            new = [development[midi]["layers"][str(velocity)]["correctedAbsoluteLogError"] for midi in midis]
            pairs = [(a, b) for a, b in zip(base, new) if a is not None and b is not None]
            by_layer[str(velocity)] = {
                "baselineAbsoluteLogError": percentile_summary([a for a, _ in pairs]),
                "correctedAbsoluteLogError": percentile_summary([b for _, b in pairs]),
                "improvedFraction": sum(b < a for a, b in pairs) / len(pairs) if pairs else None,
            }
            baseline.extend(a for a, _ in pairs)
            corrected.extend(b for _, b in pairs)
        return {
            "baselineAbsoluteLogError": percentile_summary(baseline),
            "correctedAbsoluteLogError": percentile_summary(corrected),
            "improvedFraction": sum(b < a for a, b in zip(baseline, corrected)) / len(baseline) if baseline else None,
            "byLayer": by_layer,
        }
    return {
        "status": "development_only_not_final_validation",
        "allHeldOutKeys": summarize(sorted(development)),
        "byRegister": {name: summarize(sorted(midi for midi in development if lower <= midi <= upper)) for name, (lower, upper) in REGISTERS.items()},
    }


def render_dry(midi: int, velocity: int) -> Path:
    RENDER_ROOT.mkdir(parents=True, exist_ok=True)
    output = RENDER_ROOT / f"midi-{midi}-layer-{velocity}.wav"
    command = [
        str(RENDERER), str(output), str(midi), format(VELOCITY_ANCHORS[velocity], ".15g"), "3", "1", "0", "0",
    ]
    completed = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if completed.returncode != 0:
        raise RuntimeError(f"Dry render failed for MIDI {midi} layer {velocity}: {completed.stderr}")
    return output


def radiation_feature(stereo: np.ndarray, sample_rate: int, region: int) -> dict:
    onset = find_onset(stereo.mean(axis=1), sample_rate)
    frequencies = BODY_MODE_FREQUENCIES * (1.0 + 0.018 * (region - 1.0))
    windows = {
        "early": (0.04, 0.28),
        "sustained": (0.40, 1.00),
    }
    result = {}
    for name, (start_offset, stop_offset) in windows.items():
        start = onset + int(start_offset * sample_rate)
        stop = min(len(stereo), onset + int(stop_offset * sample_rate))
        window = stereo[start:stop]
        if window.shape[0] < int(0.1 * sample_rate):
            raise RuntimeError("Insufficient radiation analysis window")
        level = float(np.sqrt(np.mean(window * window)))
        hann = np.hanning(window.shape[0])
        nfft = 1 << int(math.ceil(math.log2(window.shape[0] * 4)))
        spectrum = np.fft.rfft(window * hann[:, None], nfft, axis=0)
        power = np.sum(np.abs(spectrum) ** 2, axis=1)
        bins = np.fft.rfftfreq(nfft, 1.0 / sample_rate)
        mode_power = []
        for frequency in frequencies:
            if frequency > 12000.0:
                continue
            half_width = max(3.0, 0.025 * frequency)
            band = np.abs(bins - frequency) <= half_width
            value = float(np.sum(power[band]))
            if value > 1.0e-20:
                mode_power.append({"frequency": float(frequency), "power": value, "x": math.log(frequency / 500.0)})
        result[name] = {"levelRms": level, "modePower": mode_power}
    return result


def radiation_observation(midi: int, velocity: int, model_path: Path) -> dict:
    recorded, sample_rate = read_stereo(audio_path(midi, velocity), velocity)
    rendered, rendered_rate = read_stereo(model_path)
    if rendered_rate != sample_rate:
        raise RuntimeError("Renderer/sample rate mismatch")
    region = REGION_INDEX[register_name(midi)]
    recorded_feature = radiation_feature(recorded, sample_rate, region)
    rendered_feature = radiation_feature(rendered, rendered_rate, region)
    observation = {"midi": midi, "velocityLayer": velocity, "windows": {}}
    for window in ("early", "sustained"):
        reference = recorded_feature[window]
        model = rendered_feature[window]
        mode_ratios = []
        model_by_frequency = {round(item["frequency"], 6): item for item in model["modePower"]}
        for item in reference["modePower"]:
            other = model_by_frequency.get(round(item["frequency"], 6))
            if other and item["power"] > 1.0e-20 and other["power"] > 1.0e-20:
                mode_ratios.append({
                    "frequency": item["frequency"],
                    "x": item["x"],
                    "logRatio": 0.5 * math.log(item["power"] / other["power"]),
                })
        observation["windows"][window] = {
            "levelLogRatio": math.log(max(reference["levelRms"], 1.0e-20) / max(model["levelRms"], 1.0e-20)),
            "modeRatios": mode_ratios,
        }
    return observation


def improvement_summary(observations: list[dict], direct: float, resonance: float, tilt: float) -> dict:
    level_base, level_new, mode_base, mode_new = [], [], [], []
    per_observation = []
    for observation in observations:
        bases, candidates = [], []
        for window in observation["windows"].values():
            level = window["levelLogRatio"]
            level_base.append(abs(level))
            level_new.append(abs(level - math.log(direct)))
            for mode in window["modeRatios"]:
                prediction = math.log(direct) + math.log(max(resonance, 1.0e-20)) + tilt * mode["x"]
                mode_base.append(abs(mode["logRatio"]))
                mode_new.append(abs(mode["logRatio"] - prediction))
                bases.append(abs(mode["logRatio"]))
                candidates.append(abs(mode["logRatio"] - prediction))
        base_mean = float(np.mean(bases)) if bases else 0.0
        candidate_mean = float(np.mean(candidates)) if candidates else 0.0
        per_observation.append({
            "midi": observation["midi"], "velocityLayer": observation["velocityLayer"],
            "baselineModeMeanAbsoluteError": base_mean, "candidateModeMeanAbsoluteError": candidate_mean,
            "improved": candidate_mean < base_mean,
        })
    return {
        "levelAbsoluteLogError": percentile_summary(level_base),
        "candidateLevelAbsoluteLogError": percentile_summary(level_new),
        "levelImprovedFraction": sum(new < old for old, new in zip(level_base, level_new)) / len(level_base) if level_base else None,
        "modeAbsoluteLogError": percentile_summary(mode_base),
        "candidateModeAbsoluteLogError": percentile_summary(mode_new),
        "observations": per_observation,
        "observationImprovedFraction": sum(item["improved"] for item in per_observation) / len(per_observation) if per_observation else 0.0,
        "improvedKeyCount": len({item["midi"] for item in per_observation if item["improved"]}),
    }


def fit_body_region(name: str, observations: list[dict]) -> dict:
    levels = [window["levelLogRatio"] for observation in observations for window in observation["windows"].values()]
    median_level = float(np.median(levels))
    direct_candidate = float(np.clip(math.exp(0.5 * median_level), *DIRECT_BOUNDS))
    direct_reasons = []
    if not DIRECT_BOUNDS[0] <= math.exp(0.5 * median_level) <= DIRECT_BOUNDS[1]:
        direct_reasons.append("required_direct_gain_outside_bounds")
    xs, ys = [], []
    for observation in observations:
        for window in observation["windows"].values():
            for mode in window["modeRatios"]:
                xs.append(mode["x"])
                ys.append(mode["logRatio"] - math.log(direct_candidate))
    x = np.asarray(xs)
    y = np.asarray(ys)
    regression = least_squares(
        lambda parameters: parameters[0] + parameters[1] * x - y,
        [0.0, 0.0], bounds=([math.log(0.01), TILT_BOUNDS[0]], [math.log(RESONANCE_BOUNDS[1]), TILT_BOUNDS[1]]),
        loss="soft_l1", f_scale=0.5, max_nfev=100,
    )
    resonance_candidate = math.exp(float(regression.x[0]))
    tilt_candidate = float(regression.x[1])
    metrics = improvement_summary(observations, direct_candidate, resonance_candidate, tilt_candidate)
    if metrics["observationImprovedFraction"] < 0.65 or metrics["improvedKeyCount"] < 2:
        direct_reasons.append("insufficient_cross_key_residual_improvement")
    if resonance_candidate <= 0.010001 or resonance_candidate >= 1.999999 or abs(tilt_candidate) >= 0.999999:
        direct_reasons.append("resonance_or_tilt_at_boundary")
    for window_name in ("early", "sustained"):
        subset = [{
            "midi": observation["midi"],
            "velocityLayer": observation["velocityLayer"],
            "windows": {window_name: observation["windows"][window_name]},
        } for observation in observations]
        window_metrics = improvement_summary(subset, direct_candidate, resonance_candidate, tilt_candidate)
        if window_metrics["observationImprovedFraction"] < 0.60:
            direct_reasons.append(f"insufficient_{window_name}_window_improvement")
    for velocity in PERMITTED_AUDIO_LAYERS:
        subset = [observation for observation in observations if observation["velocityLayer"] == velocity]
        layer_metrics = improvement_summary(subset, direct_candidate, resonance_candidate, tilt_candidate)
        if layer_metrics["observationImprovedFraction"] < 0.60:
            direct_reasons.append(f"insufficient_layer_{velocity}_improvement")
    supported = not direct_reasons
    return {
        "register": name,
        "regionIndex": REGION_INDEX[name],
        "fittingKeys": BODY_FITTING_KEYS[name],
        "observationCount": len(observations),
        "supported": supported,
        "directGain": direct_candidate if supported else 1.0,
        "resonanceGain": resonance_candidate if supported else 1.0,
        "spectralTilt": tilt_candidate if supported else 0.0,
        "candidate": {"directGain": direct_candidate, "resonanceGain": resonance_candidate, "spectralTilt": tilt_candidate},
        "medianLevelLogRatio": median_level,
        "rejectionReasons": list(dict.fromkeys(direct_reasons)),
        "fittingMetrics": metrics,
    }


def body_metrics(development: list[dict], body_fits: dict[str, dict]) -> dict:
    result = {}
    for name in REGISTERS:
        fit = body_fits[name]
        subset = [observation for observation in development if register_name(observation["midi"]) == name]
        baseline = improvement_summary(subset, 1.0, 1.0, 0.0)
        fitted = improvement_summary(subset, fit["directGain"], fit["resonanceGain"], fit["spectralTilt"])
        result[name] = {
            "observationCount": len(subset),
            "baseline": baseline,
            "calibrated": fitted,
            "levelImprovedFraction": fitted["levelImprovedFraction"],
            "modeObservationImprovedFraction": fitted["observationImprovedFraction"],
        }
    return {"status": "development_only_not_final_validation", "byRegister": result}


def rounded(value):
    if isinstance(value, dict):
        return {key: rounded(item) for key, item in value.items()}
    if isinstance(value, list):
        return [rounded(item) for item in value]
    if isinstance(value, (bool, np.bool_)):
        return bool(value)
    if isinstance(value, (np.floating, float)):
        return round(float(value), 10) if math.isfinite(value) else None
    if isinstance(value, (np.integer, int)):
        return int(value)
    return value


def make_header(keys: list[dict], body_fits: dict[str, dict]) -> str:
    lines = [
        "#pragma once", "", "#include <array>", "#include <cstddef>", "#include <cstdint>", "",
        "namespace pianocraft {", "", "inline constexpr std::size_t calibrationPartialLimit = 112;", "",
        "struct PartialCalibration {", "    std::uint16_t partial;", "    double frequency;", "    double decaySeconds;",
        "    double secondaryFrequency;", "    double secondaryDecaySeconds;", "    double secondaryGain;",
        "    bool frequencyMeasured;", "    bool decayMeasured;", "    bool secondaryAccepted;", "};", "",
        "struct CalibrationData {", "    double fundamental;", "    double inharmonicity;", "    double decaySeconds;",
        "    double loudness;", "    double hammerStiffness;", "    double velocityGainExponent;",
        "    double velocityBrightnessExponent;", "    double hammerStiffnessScale;", "    double stereoLeftGain;",
        "    double stereoRightGain;", "    std::uint16_t partialCount;",
        "    std::array<PartialCalibration, calibrationPartialLimit> partials;", "};", "",
        "struct BodyCalibration {", "    std::array<double, 3> directGain;", "    std::array<double, 3> resonanceGain;",
        "    std::array<double, 3> spectralTilt;", "};", "",
    ]
    ordered = [body_fits[name] for name in ("bass", "middle", "treble")]
    lines.append("inline constexpr BodyCalibration soundboardCalibration{")
    for field in ("directGain", "resonanceGain", "spectralTilt"):
        values = ", ".join(format(fit[field], ".12g") for fit in ordered)
        lines.append("    {{" + values + "}},")
    lines.extend(["};", "", "inline constexpr bool hasMeasuredCalibration = true;", "inline constexpr std::array<CalibrationData, 88> measuredCalibration{{"])
    fields = LEGACY_FIELDS + VELOCITY_FIELDS + STEREO_FIELDS
    for key in keys:
        scalars = ", ".join(format(key["values"][field], ".12g") for field in fields)
        lines.append(f"    CalibrationData{{{scalars}, {len(key['partials'])}, {{{{")
        for partial in key["partials"]:
            values = [
                str(partial["partial"]), format(partial["frequency"], ".12g"), format(partial["decaySeconds"], ".12g"),
                format(partial["secondaryFrequency"], ".12g"), format(partial["secondaryDecaySeconds"], ".12g"),
                format(partial["secondaryGain"], ".12g"), "true" if partial["frequencyMeasured"] else "false",
                "true" if partial["decayMeasured"] else "false", "true" if partial["secondaryAccepted"] else "false",
            ]
            lines.append("        PartialCalibration{" + ", ".join(values) + "},")
        lines.append("    }}}, // MIDI " + str(key["midi"]) + " " + note_name(key["midi"]))
    lines.extend(["}};", "", "} // namespace pianocraft", ""])
    return "\n".join(lines)


def main() -> None:
    provenance = json.loads(PROVENANCE.read_text())
    frozen = load_frozen_report()
    keys = copy.deepcopy(frozen["keys"])
    held_out = set(provenance["calibration"]["held_out_midi_notes"])
    fitting_midis = [midi for midi in range(MIN_MIDI, MAX_MIDI + 1) if midi not in held_out]
    held_midis = [midi for midi in range(MIN_MIDI, MAX_MIDI + 1) if midi in held_out]
    by_midi = {key["midi"]: key for key in keys}
    source = verify_corpus(provenance)
    if not RENDERER.is_file():
        raise RuntimeError("The prebuilt offline renderer is missing")
    renderer_hash = sha256(RENDERER)
    print("Frozen modal/velocity inputs verified; layer 2 remains untouched.", flush=True)

    stereo_features = {}
    for index, midi in enumerate(fitting_midis, 1):
        stereo_features[midi] = {velocity: sustained_stereo_feature(midi, velocity) for velocity in PERMITTED_AUDIO_LAYERS}
        print(f"Stereo fit {index:02d}/{len(fitting_midis)} MIDI {midi}", flush=True)
    stereo_values, stereo_fit_report = fit_stereo(fitting_midis, stereo_features)
    for midi in fitting_midis:
        left, right = stereo_values[midi]
        by_midi[midi]["values"]["stereoLeftGain"] = left
        by_midi[midi]["values"]["stereoRightGain"] = right
        by_midi[midi]["stereoFit"] = {"source": "fitting_layers_1_3_4", "features": stereo_features[midi]}
    for midi in held_midis:
        lower_midi = max(key for key in fitting_midis if key < midi)
        upper_midi = min(key for key in fitting_midis if key > midi)
        left, right = interpolate_stereo(midi, by_midi[lower_midi], by_midi[upper_midi])
        by_midi[midi]["values"]["stereoLeftGain"] = left
        by_midi[midi]["values"]["stereoRightGain"] = right
        by_midi[midi]["stereoFit"] = {"source": "neighbor_log_interpolation", "lowerMidi": lower_midi, "upperMidi": upper_midi}

    stereo_dev = {}
    for midi in held_midis:
        features = {velocity: sustained_stereo_feature(midi, velocity) for velocity in PERMITTED_AUDIO_LAYERS}
        gains = (by_midi[midi]["values"]["stereoLeftGain"], by_midi[midi]["values"]["stereoRightGain"])
        stereo_dev[midi] = stereo_development(midi, features, gains)
        by_midi[midi]["stereoDevelopment"] = stereo_dev[midi]
    stereo_dev_metrics = stereo_metrics(stereo_dev)

    body_observations = {name: [] for name in REGISTERS}
    fitting_body_keys = [midi for name in REGISTERS for midi in BODY_FITTING_KEYS[name]]
    for index, midi in enumerate(fitting_body_keys, 1):
        for velocity in PERMITTED_AUDIO_LAYERS:
            model_path = render_dry(midi, velocity)
            body_observations[register_name(midi)].append(radiation_observation(midi, velocity, model_path))
        print(f"Body fit {index:02d}/{len(fitting_body_keys)} MIDI {midi}", flush=True)
    body_fits = {name: fit_body_region(name, body_observations[name]) for name in REGISTERS}

    body_dev_observations = []
    for index, midi in enumerate(held_midis, 1):
        for velocity in PERMITTED_AUDIO_LAYERS:
            model_path = render_dry(midi, velocity)
            body_dev_observations.append(radiation_observation(midi, velocity, model_path))
        print(f"Body development {index:02d}/{len(held_midis)} MIDI {midi}", flush=True)
    body_dev_metrics = body_metrics(body_dev_observations, body_fits)

    if partial_digest(keys) != FROZEN_PARTIAL_DIGEST:
        raise RuntimeError("Per-partial data changed during body/stereo generation")
    header = make_header(keys, body_fits)
    HEADER.write_text(header)
    body_arrays = {
        field: [body_fits[name][field] for name in ("bass", "middle", "treble")]
        for field in ("directGain", "resonanceGain", "spectralTilt")
    }
    report = {
        "schemaVersion": 4,
        "algorithmVersion": ALGORITHM_VERSION,
        "source": source,
        "software": {"numpy": np.__version__, "scipy": scipy_version},
        "definitions": {
            **frozen["definitions"],
            "bodyFittingKeys": BODY_FITTING_KEYS,
            "bodyDevelopmentKeys": held_midis,
            "bodyAndStereoLayers": list(PERMITTED_AUDIO_LAYERS),
            "bodyRegionOrder": ["bass MIDI21..47", "middle MIDI48..71", "treble MIDI72..108"],
            "renderer": {"sha256": renderer_hash, "velocityAnchors": VELOCITY_ANCHORS, "seconds": 3, "pedal": 1, "body": 0, "sympathy": 0},
            "frozenInputs": "algorithm-3 modal data and algorithm-4 register velocity parameters; neither was refitted",
        },
        "methods": {
            **frozen["methods"],
            "bodyResidual": "dry-render/reference early and sustained RMS and log body-mode-band residuals; robust regional gain/tilt candidate with cross-key, cross-window, and cross-layer acceptance checks",
            "bodyFallback": "neutral direct=1, resonance=1, tilt=0 unless the bounded candidate improves at least 65% of fitting observations and both windows/all layers without boundary solutions",
            "stereo": "sustained L/R RMS balance relative to existing constant-power pan; layer median, within-register Gaussian smoothing, 0.35 shrinkage, and geometric-mean-preserving left/right corrections",
            "stereoScope": "effective balance only; not physical key positions, phase, microphone response, or stereo correlation",
        },
        "parameterBounds": {
            **frozen["parameterBounds"],
            "directGain": list(DIRECT_BOUNDS), "resonanceGain": list(RESONANCE_BOUNDS), "spectralTilt": list(TILT_BOUNDS),
            "stereoLeftGain": list(STEREO_BOUNDS), "stereoRightGain": list(STEREO_BOUNDS),
        },
        "registerExcitationFits": frozen["registerExcitationFits"],
        "soundboardCalibration": body_arrays,
        "bodyFits": body_fits,
        "stereoFits": stereo_fit_report,
        "qualitySummary": frozen["qualitySummary"],
        "heldOutDevelopmentMetrics": frozen["heldOutDevelopmentMetrics"],
        "excitationDevelopmentMetrics": frozen["excitationDevelopmentMetrics"],
        "bodyDevelopmentMetrics": body_dev_metrics,
        "stereoDevelopmentMetrics": stereo_dev_metrics,
        "finalLayer2Validation": frozen["finalLayer2Validation"],
        "frozenPartialDigest": FROZEN_PARTIAL_DIGEST,
        "generatedHeaderSha256": hashlib.sha256(header.encode()).hexdigest(),
        "limitations": [
            "Body parameters are effective residual radiation corrections, not identified soundboard geometry, material, poles, or damping.",
            "Dry residual similarity is limited by frozen string and recording differences; unsupported fits remain neutral rather than extrapolating to note-specific noise or room tails.",
            "Stereo gains are conservative sustained balance corrections and do not recover physical positions, phase, microphone response, or correlation.",
            "Every-third-key body and stereo development uses layers 1, 3, and 4 only; final layer-2 validation remains pending and unread.",
            "Analysis renders are derived local artifacts only; no raw recordings are bundled.",
        ],
        "keys": keys,
    }
    REPORT.write_text(json.dumps(rounded(report), indent=2, ensure_ascii=False) + "\n")
    print(json.dumps({"body": body_arrays, "stereo": stereo_fit_report}, indent=2), flush=True)
    print(json.dumps({"bodyDevelopment": body_dev_metrics, "stereoDevelopment": stereo_dev_metrics["allHeldOutKeys"]}, indent=2), flush=True)
    print(f"Wrote {HEADER.relative_to(ROOT)} and {REPORT.relative_to(ROOT)}", flush=True)


if __name__ == "__main__":
    main()
