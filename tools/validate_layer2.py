#!/usr/bin/env python3
"""Evaluate the frozen PianoCraft model on layer 2 without fitting or tuning."""

from __future__ import annotations

import hashlib
import json
import math
import subprocess
import warnings
from collections import Counter
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.io.wavfile import WavFileWarning
from scipy.ndimage import uniform_filter1d
from scipy.optimize import least_squares, minimize_scalar
from scipy.signal import butter, hilbert, sosfiltfilt


ROOT = Path(__file__).resolve().parents[1]
AUDIO_ROOT = ROOT / "calibration" / "meta-piano"
RENDER_ROOT = ROOT / "calibration" / "layer2-validation"
RENDERER = ROOT / "build" / "pianocraft-render"
REPORT = ROOT / "model" / "calibration-report.json"
HEADER = ROOT / "src" / "model" / "pianos" / "meta" / "CalibrationData.hpp"
SCRIPT = ROOT / "tools" / "validate_layer2.py"
VELOCITY = 60.0 / 127.0
BODY_MODE_FREQUENCIES = np.array([72.0, 103.0, 149.0, 213.0, 307.0, 441.0, 635.0, 914.0, 1316.0, 1895.0, 2728.0, 3928.0, 5656.0, 8144.0])
REGIONS = {"bass": (21, 47), "middle": (48, 71), "treble": (72, 108)}


def note_name(midi: int) -> str:
    names = ("C", "Cs", "D", "Ds", "E", "F", "Fs", "G", "Gs", "A", "As", "B")
    return f"{names[midi % 12]}{midi // 12 - 1}"


def audio_path(midi: int) -> Path:
    return AUDIO_ROOT / f"INSPiano_Vel02_{note_name(midi)}_01.wav"


def region_name(midi: int) -> str:
    return next(name for name, (lower, upper) in REGIONS.items() if lower <= midi <= upper)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def partial_digest(keys: list[dict]) -> str:
    payload = [[key["midi"], key["partials"]] for key in keys]
    return hashlib.sha256(json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def read_audio(path: Path) -> tuple[np.ndarray, int]:
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


def parabolic_frequency(spectrum: np.ndarray, index: int, bin_hz: float) -> float:
    if index <= 0 or index >= spectrum.size - 1:
        return float(index * bin_hz)
    values = np.log(np.maximum(spectrum[index - 1 : index + 2], 1.0e-30))
    denominator = values[0] - 2.0 * values[1] + values[2]
    offset = 0.0 if abs(denominator) < 1.0e-20 else 0.5 * (values[0] - values[2]) / denominator
    return float((index + np.clip(offset, -1.0, 1.0)) * bin_hz)


def analysis_spectrum(mono: np.ndarray, sample_rate: int, onset: int, midi: int) -> dict:
    if midi >= 104:
        duration, attack_skip = 0.45, 0.012
    elif midi >= 96:
        duration, attack_skip = 0.8, 0.02
    else:
        duration, attack_skip = 2.0, 0.06
    start = min(onset + int(attack_skip * sample_rate), len(mono) - 1)
    stop = min(len(mono), start + int(duration * sample_rate))
    segment = mono[start:stop]
    if segment.size < int(0.1 * sample_rate):
        raise RuntimeError(f"Insufficient layer-2 analysis window for MIDI {midi}")
    segment = segment - segment.mean()
    window = np.hanning(segment.size)
    nfft = 1 << int(math.ceil(math.log2(segment.size * 4)))
    spectrum = np.abs(np.fft.rfft(segment * window, nfft)) * (2.0 / window.sum())
    return {
        "segment": segment,
        "window": window,
        "spectrum": spectrum,
        "frequencies": np.fft.rfftfreq(nfft, 1.0 / sample_rate),
        "binHz": sample_rate / nfft,
        "sampleRate": sample_rate,
    }


def measure_fundamental(analysis: dict, midi: int) -> dict | None:
    expected = 440.0 * 2.0 ** ((midi - 69) / 12.0)
    spectrum, frequencies = analysis["spectrum"], analysis["frequencies"]
    search = np.flatnonzero(
        (frequencies >= expected * 2.0 ** (-80.0 / 1200.0))
        & (frequencies <= expected * 2.0 ** (80.0 / 1200.0))
    )
    peak_index = int(search[np.argmax(spectrum[search])])
    peak = float(spectrum[peak_index])
    excluded = np.abs(frequencies[search] - frequencies[peak_index]) < max(3 * analysis["binHz"], 0.004 * expected)
    noise = float(np.median(spectrum[search][~excluded])) if np.any(~excluded) else float(np.median(spectrum[search]))
    snr = 20.0 * math.log10(max(peak, 1.0e-30) / max(noise, 1.0e-30))
    if peak_index - search[0] < 2 or search[-1] - peak_index < 2 or snr < 12.0:
        return None
    times = np.arange(analysis["segment"].size, dtype=np.float64) / analysis["sampleRate"]
    weighted = analysis["segment"] * analysis["window"]
    first, last = max(int(search[0]), peak_index - 2), min(int(search[-1]), peak_index + 2)

    def objective(frequency: float) -> float:
        return -float(abs(np.sum(weighted * np.exp(-2j * np.pi * frequency * times))))

    refined = minimize_scalar(
        objective, bounds=(float(frequencies[first]), float(frequencies[last])), method="bounded",
        options={"xatol": 1.0e-6, "maxiter": 32},
    )
    frequency = float(refined.x if refined.success else parabolic_frequency(spectrum, peak_index, analysis["binHz"]))
    cents = 1200.0 * math.log2(frequency / expected)
    if abs(cents) > 75.0:
        return None
    return {"frequency": frequency, "snrDb": snr}


def measure_partial(analysis: dict, expected: float, tonal_maximum: float) -> dict | None:
    spectrum, frequencies = analysis["spectrum"], analysis["frequencies"]
    tolerance = max(3.0 * analysis["binHz"], 0.004 * expected)
    band = np.flatnonzero(np.abs(frequencies - expected) <= tolerance)
    if band.size < 3:
        return None
    peak_index = int(band[np.argmax(spectrum[band])])
    peak = float(spectrum[peak_index])
    outer = np.flatnonzero(
        (np.abs(frequencies - expected) <= 0.012 * expected)
        & (np.abs(frequencies - expected) >= 0.006 * expected)
    )
    noise = float(np.median(spectrum[outer])) if outer.size else float(np.median(spectrum[band]))
    snr = 20.0 * math.log10(max(peak, 1.0e-30) / max(noise, 1.0e-30))
    frequency = parabolic_frequency(spectrum, peak_index, analysis["binHz"])
    if snr < 12.0 or peak < tonal_maximum * 0.001 or abs(frequency / expected - 1.0) >= 0.006:
        return None
    return {"frequency": frequency, "snrDb": snr}


def robust_line(times: np.ndarray, levels: np.ndarray) -> tuple[float, float]:
    slope, intercept = np.polyfit(times, levels, 1)
    fit = least_squares(
        lambda parameters: parameters[0] * times + parameters[1] - levels,
        [slope, intercept], loss="soft_l1", f_scale=2.0, max_nfev=100,
    )
    return float(fit.x[0]), float(fit.x[1])


def reference_decay(mono: np.ndarray, sample_rate: int, onset: int, midi: int, fundamental: float, frequency: float) -> dict:
    result = {"accepted": False, "t60Seconds": None, "fitDurationSeconds": None, "residualDbRms": None, "rejectionReasons": []}
    half_width = max(1.5, min(0.004 * frequency, 0.12 * fundamental))
    low, high = max(1.0, frequency - half_width), min(0.47 * sample_rate, frequency + half_width)
    if high <= low * 1.001:
        result["rejectionReasons"].append("invalid_filter_band")
        return result
    try:
        sos = butter(3, [low, high], btype="bandpass", fs=sample_rate, output="sos")
        envelope = np.abs(hilbert(sosfiltfilt(sos, mono)))
    except (ValueError, FloatingPointError):
        result["rejectionReasons"].append("filter_failure")
        return result
    envelope = uniform_filter1d(envelope, max(1, int((0.01 if midi >= 96 else 0.025) * sample_rate)))
    search_start = min(onset + int((0.015 if midi >= 96 else 0.06) * sample_rate), len(envelope) - 1)
    search_stop = min(len(envelope), search_start + int((0.12 if midi >= 96 else 0.3) * sample_rate))
    if search_stop - search_start < 8:
        result["rejectionReasons"].append("attack_window_boundary")
        return result
    peak_index = search_start + int(np.argmax(envelope[search_start:search_stop]))
    peak = float(envelope[peak_index])
    noise = float(np.median(envelope[max(peak_index + 1, len(envelope) - int(0.15 * sample_rate)) :]))
    peak_to_noise = 20.0 * math.log10(max(peak, 1.0e-30) / max(noise, 1.0e-30))
    result["peakToNoiseDb"] = peak_to_noise
    if peak <= 1.0e-12 or peak_to_noise < 18.0:
        result["rejectionReasons"].append("insufficient_snr")
        return result
    start_level = peak * 10.0 ** (-5.0 / 20.0)
    end_level = max(peak * 10.0 ** (-25.0 / 20.0), noise * 5.0)
    if end_level >= start_level:
        result["rejectionReasons"].append("insufficient_decay_dynamic_range")
        return result
    after_peak = np.flatnonzero(envelope[peak_index:] <= start_level)
    if not after_peak.size:
        result["rejectionReasons"].append("start_boundary")
        return result
    start = peak_index + int(after_peak[0])
    after_start = np.flatnonzero(envelope[start:] <= end_level)
    hard_stop = len(envelope) - min(int(0.2 * sample_rate), max(1, (len(envelope) - start) // 10))
    end = min(start + int(after_start[0]) if after_start.size else hard_stop, hard_stop)
    if not after_start.size or end >= hard_stop:
        result["rejectionReasons"].append("end_boundary")
    if end <= start:
        if "end_boundary" not in result["rejectionReasons"]:
            result["rejectionReasons"].append("end_boundary")
        return result
    step = max(1, int((0.005 if midi >= 96 else 0.01) * sample_rate))
    indices = np.arange(start, end, step, dtype=int)
    if indices.size < 8:
        indices = np.linspace(start, end - 1, 8, dtype=int)
    times = (indices - start) / sample_rate
    levels = 20.0 * np.log10(np.maximum(envelope[indices], peak * 1.0e-8))
    duration = float(times[-1] - times[0])
    observed_drop = float(np.median(levels[: min(3, levels.size)]) - np.median(levels[-min(3, levels.size) :]))
    result.update({"fitDurationSeconds": duration, "observedDropDb": observed_drop})
    if duration < max(0.05, 4.0 / frequency):
        result["rejectionReasons"].append("short_fit_window")
    if observed_drop < 12.0:
        result["rejectionReasons"].append("insufficient_observed_db_drop")
    if times[-1] <= times[0]:
        return result
    slope, intercept = robust_line(times, levels)
    residual = levels - (slope * times + intercept)
    residual_rms = float(np.sqrt(np.mean(residual * residual)))
    result["residualDbRms"] = residual_rms
    if residual_rms > 5.0:
        result["rejectionReasons"].append("excessive_envelope_residual")
    if slope >= -0.05:
        result["rejectionReasons"].append("non_decay_slope")
        return result
    t60 = -60.0 / slope
    if not math.isfinite(t60) or t60 <= 0.0 or t60 > 180.0:
        result["rejectionReasons"].append("t60_physical_boundary")
    if math.isfinite(t60) and t60 > 4.0 * duration:
        result["rejectionReasons"].append("extrapolated_t60_over_4x_fit_duration")
    result["rejectionReasons"] = list(dict.fromkeys(result["rejectionReasons"]))
    if not result["rejectionReasons"]:
        result.update({"accepted": True, "t60Seconds": t60})
    return result


def early_feature(stereo: np.ndarray, sample_rate: int, onset: int, midi: int, fundamental: float) -> dict:
    treble = midi >= 72
    start = onset + int((0.025 if treble else 0.06) * sample_rate)
    stop = min(len(stereo), start + int((0.20 if treble else 0.35) * sample_rate))
    window = stereo[start:stop]
    level = float(np.sqrt(np.mean(window * window)))
    hann = np.hanning(window.shape[0])
    nfft = 1 << int(math.ceil(math.log2(window.shape[0] * 2)))
    spectrum = np.fft.rfft(window * hann[:, None], nfft, axis=0)
    power = np.sum(np.abs(spectrum) ** 2, axis=1)
    frequencies = np.fft.rfftfreq(nfft, 1.0 / sample_rate)
    low = (frequencies >= 0.5 * fundamental) & (frequencies < 2.0 * fundamental)
    high = (frequencies >= 2.0 * fundamental) & (frequencies < min(8.0 * fundamental, 18000.0))
    low_energy, high_energy = float(np.sum(power[low])), float(np.sum(power[high]))
    brightness = math.log(max(high_energy, 1.0e-30) / max(low_energy, 1.0e-30))
    return {"levelRms": level, "brightnessLogHighLow": brightness}


def sustained_balance(stereo: np.ndarray, sample_rate: int, onset: int, midi: int) -> float:
    treble = midi >= 72
    start = onset + int((0.03 if treble else 0.15) * sample_rate)
    stop = min(len(stereo), start + int((0.30 if treble else 0.50) * sample_rate))
    window = stereo[start:stop]
    left = float(np.sqrt(np.mean(window[:, 0] ** 2)))
    right = float(np.sqrt(np.mean(window[:, 1] ** 2)))
    return math.log(max(left, 1.0e-30) / max(right, 1.0e-30))


def radiation_feature(stereo: np.ndarray, sample_rate: int, onset: int, region: int) -> dict:
    frequencies = BODY_MODE_FREQUENCIES * (1.0 + 0.018 * (region - 1.0))
    result = {}
    for name, start_offset, stop_offset in (("early", 0.04, 0.28), ("sustained", 0.40, 1.00)):
        start, stop = onset + int(start_offset * sample_rate), min(len(stereo), onset + int(stop_offset * sample_rate))
        window = stereo[start:stop]
        level = float(np.sqrt(np.mean(window * window)))
        hann = np.hanning(window.shape[0])
        nfft = 1 << int(math.ceil(math.log2(window.shape[0] * 4)))
        spectrum = np.fft.rfft(window * hann[:, None], nfft, axis=0)
        power = np.sum(np.abs(spectrum) ** 2, axis=1)
        bins = np.fft.rfftfreq(nfft, 1.0 / sample_rate)
        modes = []
        for frequency in frequencies:
            if frequency > 12000.0:
                continue
            band = np.abs(bins - frequency) <= max(3.0, 0.025 * frequency)
            value = float(np.sum(power[band]))
            if value > 1.0e-20:
                modes.append((float(frequency), value))
        result[name] = {"levelRms": level, "modes": modes}
    return result


def render(midi: int, body: int) -> Path:
    RENDER_ROOT.mkdir(parents=True, exist_ok=True)
    output = RENDER_ROOT / f"midi-{midi}-body-{body}.wav"
    command = [str(RENDERER), str(output), str(midi), format(VELOCITY, ".16g"), "3", "1", str(body), "0"]
    completed = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if completed.returncode != 0:
        raise RuntimeError(f"Render failed for MIDI {midi}: {completed.stderr}")
    return output


def summary(values: list[float]) -> dict:
    if not values:
        return {"count": 0, "medianAbsolute": None, "p95Absolute": None, "maxAbsolute": None, "medianSigned": None}
    array = np.asarray(values, dtype=np.float64)
    absolute = np.abs(array)
    return {
        "count": int(array.size),
        "medianAbsolute": float(np.median(absolute)),
        "p95Absolute": float(np.percentile(absolute, 95)),
        "maxAbsolute": float(np.max(absolute)),
        "medianSigned": float(np.median(array)),
    }


def worst(records: list[dict], field: str = "error", count: int = 5) -> list[dict]:
    return sorted(records, key=lambda record: abs(record[field]), reverse=True)[:count]


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


def main() -> None:
    header_before = HEADER.read_bytes()
    header_hash_before = hashlib.sha256(header_before).hexdigest()
    report = json.loads(REPORT.read_text())
    if partial_digest(report["keys"]) != report["frozenPartialDigest"]:
        raise RuntimeError("Frozen per-partial digest mismatch")
    if hashlib.sha256(header_before).hexdigest() != report["generatedHeaderSha256"]:
        raise RuntimeError("Model header does not match the frozen report")
    if report["qualitySummary"]["secondaryAcceptedCount"] != 0:
        raise RuntimeError("Frozen secondary rejection result changed")
    frozen_report_digest = hashlib.sha256(
        json.dumps({key: value for key, value in report.items() if key != "finalLayer2Validation"}, sort_keys=True).encode()
    ).hexdigest()
    renderer_hash = sha256(RENDERER)
    RENDER_ROOT.mkdir(parents=True, exist_ok=True)

    fundamental_errors, frequency_records, decay_records = [], [], []
    level_records, brightness_records, stereo_records = [], [], []
    body_level = {name: [] for name in REGIONS}
    body_mode = {name: [] for name in REGIONS}
    body_key_records = []
    decay_rejections = Counter()
    per_key = []
    counts = Counter()
    actual_audio_read = False

    for midi in range(21, 109):
        key = report["keys"][midi - 21]
        stereo, sample_rate = read_audio(audio_path(midi))
        actual_audio_read = True
        mono = stereo.mean(axis=1)
        mono -= mono.mean()
        onset = find_onset(mono, sample_rate)
        analysis = analysis_spectrum(mono, sample_rate, onset, midi)
        fundamental_reference = measure_fundamental(analysis, midi)
        fundamental_error = None
        reference_frequencies = {}
        if fundamental_reference is not None:
            reference_frequencies[1] = fundamental_reference["frequency"]
            fundamental_error = 1200.0 * math.log2(fundamental_reference["frequency"] / key["values"]["fundamental"])
            fundamental_errors.append(fundamental_error)
            counts["fundamentalSupported"] += 1
        else:
            counts["fundamentalUnsupported"] += 1
        tonal = (analysis["frequencies"] >= key["values"]["fundamental"] * 0.8) & (analysis["frequencies"] <= 18000.0)
        tonal_maximum = max(float(analysis["spectrum"][tonal].max()), 1.0e-30)
        key_frequency_errors, key_decay_errors = [], []
        for partial in key["partials"]:
            k = partial["partial"]
            frequency_supported = partial["frequencyMeasured"] or partial["frequencySource"] == "neighbor_interpolation"
            reference = None
            if k == 1 and fundamental_reference is not None:
                reference = fundamental_reference
            elif frequency_supported:
                reference = measure_partial(analysis, partial["frequency"], tonal_maximum)
            if frequency_supported and reference is not None:
                reference_frequencies[k] = reference["frequency"]
                error = 1200.0 * math.log2(reference["frequency"] / partial["frequency"])
                record = {
                    "midi": midi, "note": note_name(midi), "partial": k, "error": error,
                    "calibrationSource": partial["frequencySource"], "referenceSnrDb": reference["snrDb"],
                }
                frequency_records.append(record)
                key_frequency_errors.append(error)
                counts["partialFrequencySupported"] += 1
            elif frequency_supported:
                counts["partialFrequencyReferenceUnsupported"] += 1
            decay_supported = partial["decayMeasured"] or partial["decaySource"] == "neighbor_interpolation"
            if decay_supported and k in reference_frequencies:
                fit = reference_decay(mono, sample_rate, onset, midi, reference_frequencies[1] if 1 in reference_frequencies else key["values"]["fundamental"], reference_frequencies[k])
                if fit["accepted"]:
                    error = (fit["t60Seconds"] - partial["decaySeconds"]) / partial["decaySeconds"]
                    record = {
                        "midi": midi, "note": note_name(midi), "partial": k, "error": error,
                        "referenceT60Seconds": fit["t60Seconds"], "modelT60Seconds": partial["decaySeconds"],
                        "fitDurationSeconds": fit["fitDurationSeconds"], "referenceResidualDbRms": fit["residualDbRms"],
                    }
                    decay_records.append(record)
                    key_decay_errors.append(error)
                    counts["partialDecaySupported"] += 1
                else:
                    decay_rejections.update(fit["rejectionReasons"])
                    counts["partialDecayReferenceRejected"] += 1
            elif decay_supported:
                decay_rejections["reference_frequency_unsupported"] += 1
                counts["partialDecayReferenceRejected"] += 1

        full_render, full_rate = read_audio(render(midi, 1))
        dry_render, dry_rate = read_audio(render(midi, 0))
        full_onset = find_onset(full_render.mean(axis=1), full_rate)
        dry_onset = find_onset(dry_render.mean(axis=1), dry_rate)
        reference_early = early_feature(stereo, sample_rate, onset, midi, key["values"]["fundamental"])
        model_early = early_feature(full_render, full_rate, full_onset, midi, key["values"]["fundamental"])
        level_error = math.log(max(reference_early["levelRms"], 1.0e-30) / max(model_early["levelRms"], 1.0e-30))
        brightness_error = reference_early["brightnessLogHighLow"] - model_early["brightnessLogHighLow"]
        level_records.append({"midi": midi, "note": note_name(midi), "error": level_error})
        brightness_records.append({"midi": midi, "note": note_name(midi), "error": brightness_error})
        reference_balance = sustained_balance(stereo, sample_rate, onset, midi)
        model_balance = sustained_balance(full_render, full_rate, full_onset, midi)
        stereo_records.append({"midi": midi, "note": note_name(midi), "error": reference_balance - model_balance})

        region = list(REGIONS).index(region_name(midi))
        reference_radiation = radiation_feature(stereo, sample_rate, onset, region)
        model_radiation = radiation_feature(dry_render, dry_rate, dry_onset, region)
        key_body_errors = []
        for window in ("early", "sustained"):
            reference_window, model_window = reference_radiation[window], model_radiation[window]
            body_level[region_name(midi)].append(math.log(max(reference_window["levelRms"], 1.0e-30) / max(model_window["levelRms"], 1.0e-30)))
            model_modes = {round(frequency, 6): power for frequency, power in model_window["modes"]}
            for frequency, power in reference_window["modes"]:
                model_power = model_modes.get(round(frequency, 6))
                if model_power:
                    error = 0.5 * math.log(power / model_power)
                    body_mode[region_name(midi)].append(error)
                    key_body_errors.append(error)
        body_key_records.append({"midi": midi, "note": note_name(midi), "error": float(np.mean(np.abs(key_body_errors))) if key_body_errors else 0.0})
        per_key.append({
            "midi": midi,
            "note": note_name(midi),
            "fundamentalCentsError": fundamental_error,
            "supportedPartialFrequencyCount": len(key_frequency_errors),
            "partialFrequencyMedianAbsoluteCentsError": float(np.median(np.abs(key_frequency_errors))) if key_frequency_errors else None,
            "supportedPartialDecayCount": len(key_decay_errors),
            "partialDecayMedianAbsoluteRelativeError": float(np.median(np.abs(key_decay_errors))) if key_decay_errors else None,
            "renderedLevelLogError": level_error,
            "renderedBrightnessLogError": brightness_error,
            "renderedStereoBalanceLogError": stereo_records[-1]["error"],
            "bodyMeanAbsoluteModeLogResidual": body_key_records[-1]["error"],
        })
        print(f"Layer-2 evaluation {midi - 20:02d}/88 MIDI {midi} {note_name(midi)}", flush=True)

    final_section = {
        "status": "completed_single_frozen_development_history_check",
        "audioReadOrAnalyzed": actual_audio_read,
        "actualLayer2AudioRead": actual_audio_read,
        "evaluationOnly": True,
        "modelParametersChanged": False,
        "protocol": {
            "keyCount": 88,
            "midiRange": [21, 108],
            "layer": 2,
            "effectiveVelocityAnchor": VELOCITY,
            "velocityAnchorBasis": "60/127 follows the already documented 40/127,80/127,120/127 effective-anchor spacing; it is not fitted metadata",
            "rendererSha256": renderer_hash,
            "renderSeconds": 3,
            "pedal": 1,
            "sympathy": 0,
            "fullRenderBody": 1,
            "dryResidualRenderBody": 0,
            "frequencyReference": "layer-2 Hann/DFT fundamental and local spectral peaks; only frozen direct or neighbor-supported partials",
            "decayReference": "frozen strict bandpass/Hilbert T60 criteria; boundary and T60>4*fit-duration references rejected, never clamped",
            "levelAndBrightness": "actual body=1 renders; early RMS and independent log high/low-band power ratios using frozen fundamentals",
            "stereo": "actual body=1 rendered sustained L/R RMS balance including frozen stereo corrections",
            "bodyResidual": "neutral-body dry-render early+sustained level and existing body-mode-band residuals; no fitting",
            "modelHeaderSha256Before": header_hash_before,
            "validationScriptSha256": sha256(SCRIPT),
        },
        "counts": {
            "keysEvaluated": len(per_key),
            "fundamentalSupported": counts["fundamentalSupported"],
            "fundamentalUnsupported": counts["fundamentalUnsupported"],
            "partialFrequencySupported": counts["partialFrequencySupported"],
            "partialFrequencyReferenceUnsupported": counts["partialFrequencyReferenceUnsupported"],
            "partialDecaySupported": counts["partialDecaySupported"],
            "partialDecayReferenceRejected": counts["partialDecayReferenceRejected"],
            "decayReferenceRejectionReasons": dict(sorted(decay_rejections.items())),
            "renderedLevelBrightnessStereoKeys": len(level_records),
        },
        "metrics": {
            "fundamentalCentsError": summary(fundamental_errors),
            "partialFrequencyCentsError": summary([record["error"] for record in frequency_records]),
            "partialDecayRelativeError": summary([record["error"] for record in decay_records]),
            "renderedLevelLogError": summary([record["error"] for record in level_records]),
            "renderedBrightnessLogError": summary([record["error"] for record in brightness_records]),
            "renderedStereoBalanceLogError": summary([record["error"] for record in stereo_records]),
            "bodyResidual": {
                name: {
                    "levelLogError": summary(body_level[name]),
                    "modeLogResidual": summary(body_mode[name]),
                }
                for name in REGIONS
            },
        },
        "worstKeys": {
            "partialFrequency": worst(frequency_records),
            "partialDecay": worst(decay_records),
            "renderedLevel": worst(level_records),
            "renderedBrightness": worst(brightness_records),
            "renderedStereoBalance": worst(stereo_records),
            "bodyResidual": worst(body_key_records),
        },
        "perKey": per_key,
        "limitations": [
            "Earlier layer-2 exposure means this single frozen check belongs to the project development history and is not a pristine external test.",
            "The numeric layer-2 velocity is an effective anchor, not published recording metadata; absolute level errors include corpus/renderer gain-scale differences.",
            "Frequency and decay metrics include only supported reference measurements; rejected or extrapolated decays are counted, not silently accepted.",
            "Brightness, stereo, and body residuals are effective recording/render comparisons, not identifications of physical hammer, microphone, soundboard, or room behavior.",
            "No model parameter, threshold, fallback, interpolation, or secondary acceptance was changed by this evaluation.",
            "Validation renders are derived local artifacts; no raw layer-2 audio is bundled.",
        ],
        "modelHeaderSha256After": None,
        "modelHeaderUnchanged": None,
    }
    final_section["modelHeaderSha256After"] = hashlib.sha256(HEADER.read_bytes()).hexdigest()
    final_section["modelHeaderUnchanged"] = HEADER.read_bytes() == header_before
    if not final_section["modelHeaderUnchanged"]:
        raise RuntimeError("Model header changed during validation")
    report["finalLayer2Validation"] = rounded(final_section)
    if hashlib.sha256(json.dumps({key: value for key, value in report.items() if key != "finalLayer2Validation"}, sort_keys=True).encode()).hexdigest() != frozen_report_digest:
        raise RuntimeError("A report section outside finalLayer2Validation changed")
    REPORT.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    if HEADER.read_bytes() != header_before:
        raise RuntimeError("Model header changed after report writing")
    print(json.dumps({"counts": final_section["counts"], "metrics": final_section["metrics"]}, indent=2), flush=True)
    print(f"Layer-2 evaluation complete; model header unchanged: {header_hash_before}", flush=True)


if __name__ == "__main__":
    main()
