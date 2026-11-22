#!/usr/bin/env python3
"""Score the current renderer against the Meta Piano corpus and re-derive the fitted model laws.

Refits use layers 1, 3 and 4 and only the keys outside model/provenance.json's every-third-key
split. Layer 2 and the split keys are reported separately; both have guided earlier iterations, so
they are development evidence, not an untouched test. See docs/model-laws.md.

    .venv/bin/python tools/evaluate_model.py            # metrics
    .venv/bin/python tools/evaluate_model.py --refit    # metrics plus closed-form law refits

Set PIANOCRAFT_RENDERER to score another renderer build.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import warnings
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.io.wavfile import WavFileWarning
from scipy.optimize import least_squares
from scipy.signal import butter, hilbert, sosfilt, sosfiltfilt

warnings.filterwarnings("ignore", category=WavFileWarning)

ROOT = Path(__file__).resolve().parents[1]
AUDIO = ROOT / "calibration" / "meta-piano"
RENDERER = Path(os.environ.get("PIANOCRAFT_RENDERER", ROOT / "build" / "pianocraft-render"))
HEADER = ROOT / "src" / "model" / "CalibrationData.hpp"
VELOCITY = {1: 40 / 127, 2: 60 / 127, 3: 80 / 127, 4: 120 / 127}
NAMES = ("C", "Cs", "D", "Ds", "E", "F", "Fs", "G", "Gs", "A", "As", "B")
SCRATCH = Path(tempfile.mkdtemp(prefix="pianocraft-eval-"))
HELD_OUT_KEYS = set(json.loads((ROOT / "model" / "provenance.json").read_text())["calibration"]["held_out_midi_notes"])
FIT_KEYS = [midi for midi in range(21, 109) if midi not in HELD_OUT_KEYS]


def note_name(midi: int) -> str:
    return f"{NAMES[midi % 12]}{midi // 12 - 1}"


def fundamental(midi: int) -> float:
    return 440.0 * 2.0 ** ((midi - 69) / 12)


def calibrated_partials() -> dict[int, list[float]]:
    blocks = HEADER.read_text().split("CalibrationData{")[1:]
    return {21 + i: [float(f) for f in re.findall(r"PartialCalibration\{\d+, ([\d.e-]+)", block)]
            for i, block in enumerate(blocks)}


PARTIALS = calibrated_partials()


def read(path: Path) -> tuple[int, np.ndarray]:
    rate, data = wavfile.read(path)
    return rate, data.astype(float) / 2**31


def reference(midi: int, layer: int) -> tuple[int, np.ndarray]:
    return read(AUDIO / f"INSPiano_Vel0{layer}_{note_name(midi)}_01.wav")


def render(midi: int, layer: int, seconds: float, pedal: int = 0, body: float = 1.0,
           sympathy: float = 0.35) -> tuple[int, np.ndarray]:
    out = SCRATCH / f"{midi}-{layer}-{seconds}-{pedal}-{body}-{sympathy}.wav"
    if not out.exists():
        subprocess.run([str(RENDERER), str(out), str(midi), repr(VELOCITY[layer]), str(seconds), str(pedal),
                        str(body), str(sympathy)], check=True)
    return read(out)


def onset(mono: np.ndarray, fraction: float = 0.05) -> int:
    return int(np.argmax(np.abs(mono) > fraction * np.abs(mono).max()))


def parallel(function, items):
    with ThreadPoolExecutor(8) as executor:
        return list(executor.map(function, items))


# --- metrics ---------------------------------------------------------------------------------

BANDS = [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]


def band_powers(window: np.ndarray, rate: int) -> tuple[np.ndarray, float]:
    power = np.abs(np.fft.rfft(window * np.hanning(len(window)))) ** 2
    freq = np.fft.rfftfreq(len(window), 1 / rate)
    return np.array([power[(freq >= lo) & (freq < hi)].sum() for lo, hi in zip(BANDS[:-1], BANDS[1:])]), power.sum()


def octave_bands(rate: int, stereo: np.ndarray, floor: bool = False):
    """Octave-band levels 10-400 ms after onset, dB re the window's total. With floor, also each
    band's noise floor in the same units: the quietest such window after the first 0.4 s, short of
    the last 1.6 s, where the samples fade out. The recordings' hiss sits there, ~-84 dBFS per band."""
    left = stereo[:, 0]
    start = onset(left) + int(0.01 * rate)
    length = int(0.39 * rate)
    bands, total = band_powers(left[start:start + length], rate)
    levels = 10 * np.log10(bands / total + 1e-12)
    if not floor:
        return levels
    starts = range(start + int(0.4 * rate), len(left) - int(1.6 * rate) - length, length // 2)
    quiet = np.min([band_powers(left[s:s + length], rate)[0] for s in starts], axis=0)
    return levels, 10 * np.log10(quiet / total + 1e-12)


def spectrum_error(layers: list[int]) -> float:
    """Octave-band shape (left channel, 10-400 ms) vs reference. Below f0, and where the reference
    is within 6 dB of its noise floor (most of 8-16 kHz), only the model's excess counts."""
    def one(item):
        midi, layer = item
        model = octave_bands(*render(midi, layer, 1.0))
        ref, floor = octave_bands(*reference(midi, layer), floor=True)
        audible = ref > floor + 6
        above = np.array([hi > fundamental(midi) for hi in BANDS[1:]])
        return np.where(above & audible, model - ref, np.maximum(model - np.where(audible, ref, floor), 0.0))
    errors = np.concatenate(parallel(one, [(m, l) for m in (28, 36, 48, 60, 72, 84, 96) for l in layers]))
    return float(np.sqrt(np.mean(np.clip(errors, -40, 40) ** 2)))


def rms_level(rate: int, stereo: np.ndarray) -> float:
    start = int(np.argmax(np.abs(stereo).max(axis=1) > 0.1 * np.abs(stereo).max()))
    window = stereo[start:start + int(0.5 * rate)]
    return 20 * np.log10(np.sqrt(np.mean(window**2)))


def key_level_errors(layer: int) -> np.ndarray:
    """Per-key RMS level (first 0.5 s) relative to MIDI 48, model minus reference."""
    def one(midi):
        return rms_level(*render(midi, layer, 1.5)) - rms_level(*reference(midi, layer))
    errors = np.array(parallel(one, range(21, 109)))
    return errors - errors[48 - 21]


def sustain_errors() -> dict[int, float]:
    """Per key: RMS error of the level at 0.25-5 s relative to the first 100 ms, pedal held, layer 3."""
    times = (0.25, 0.5, 1, 2, 3, 4, 5)

    def curve(rate, stereo):
        mono = stereo.mean(axis=1)
        mono = mono[onset(mono):]
        width = int(0.1 * rate)
        peak = np.sqrt(np.mean(mono[:width] ** 2))
        return np.array([20 * np.log10(np.sqrt(np.mean(mono[int(t * rate):int(t * rate) + width] ** 2)) / peak + 1e-12)
                         if int(t * rate) + width < len(mono) else np.nan for t in times])

    def one(midi):
        diff = curve(*render(midi, 3, 6.0, pedal=1, sympathy=0.0)) - curve(*reference(midi, 3))
        return float(np.sqrt(np.mean(np.clip(diff[np.isfinite(diff)], -30, 30) ** 2)))
    keys = range(24, 109, 4)
    return dict(zip(keys, parallel(one, keys)))


def correlations(source) -> np.ndarray:
    def one(midi):
        rate, stereo = source(midi)
        start = int(np.argmax(np.abs(stereo).max(axis=1) > 0.05 * np.abs(stereo).max()))
        window = stereo[start + int(0.05 * rate):start + int(1.5 * rate)]
        return np.corrcoef(window[:, 0], window[:, 1])[0, 1]
    return np.array(parallel(one, range(24, 106, 3)))


def halo(rate: int, stereo: np.ndarray, midi: int,
         windows=((0.3, 0.6), (0.6, 1.2), (1.2, 2.4))) -> np.ndarray:
    """Energy between the note's own partials (excluding mains hum) per window, dB re band total."""
    mono = stereo.mean(axis=1)
    mono = mono[onset(mono):]
    result = []
    for start, stop in windows:
        window = mono[int(start * rate):int(stop * rate)]
        size = max(len(window), 16384)
        power = np.abs(np.fft.rfft(window * np.hanning(len(window)), size)) ** 2
        freq = np.fft.rfftfreq(size, 1 / rate)
        near = np.zeros_like(freq, bool)
        for partial in PARTIALS[midi]:
            near |= np.abs(freq - partial) < max(6, 0.012 * partial, 3 * rate / len(window))
        for base in (50.0, 60.0):
            for k in range(1, 21):
                near |= np.abs(freq - base * k) < 1.5
        band = (freq > 40) & (freq < 8000)
        result.append(10 * np.log10(power[band & ~near].sum() / power[band].sum() + 1e-20))
    return np.array(result)


def halo_error() -> np.ndarray:
    """Pedal-down render vs isolated-note recordings whose pedal state is undocumented: a plausibility
    check on the sympathetic level, not a calibration target (research roadmap section 3.4)."""
    notes = (36, 48, 57, 64, 69, 76, 84, 91)
    def one(midi):
        return halo(*render(midi, 3, 3.0, pedal=1), midi) - halo(*reference(midi, 3), midi)
    return np.array(parallel(one, notes))


def knock_error() -> float:
    """Between-partial energy 20-100 ms after the strike (hammer knock and board), model minus
    reference, layer 3, no pedal."""
    notes = (36, 48, 57, 64, 69, 76, 84, 91)
    def one(midi):
        window = ((0.02, 0.1),)
        return (halo(*render(midi, 3, 1.0), midi, window) - halo(*reference(midi, 3), midi, window))[0]
    return float(np.mean(parallel(one, notes)))


def phantom_level(rate: int, stereo: np.ndarray, midi: int) -> float:
    """Peak power at the phantom partials f_k + f_k+1 and f_k + f_k+2 that sit at least 4 Hz clear of
    every partial, relative to the partials' peak power, 150-700 Hz, 0.05-1.05 s. Above ~700 Hz the
    bass's estimated partial frequencies drift by several Hz."""
    mono = stereo.mean(axis=1)
    mono = mono[onset(mono):]
    window = mono[int(0.05 * rate):int(1.05 * rate)]
    size = 1 << 19
    power = np.abs(np.fft.rfft(window * np.hanning(len(window)), size)) ** 2
    freq = np.fft.rfftfreq(size, 1 / rate)
    partials = np.array(PARTIALS[midi])
    sums = [partials[k] + partials[k + d] for d in (1, 2) for k in range(len(partials) - d)]
    phantoms = [f for f in sums if 150 < f < 700 and np.min(np.abs(partials - f)) >= 4]
    def peaks(frequencies):
        return sum(power[np.abs(freq - f) < 1.5].max() for f in frequencies)
    return 10 * np.log10(peaks(phantoms) / peaks(partials[(partials > 150) & (partials < 700)]))


def phantom_error() -> float:
    """Phantom level, model minus reference, fitting keys A0-A1, layers 3 and 4. Higher up, too few
    phantoms clear the partials to measure."""
    items = [(m, l) for m in FIT_KEYS if m <= 33 for l in (3, 4)]
    def one(item):
        midi, layer = item
        return phantom_level(*render(midi, layer, 1.5), midi) - phantom_level(*reference(midi, layer), midi)
    return float(np.mean(parallel(one, items)))


def velocity_span() -> tuple[float, float]:
    """Level rise from layer 1 to layer 4 (first 0.5 s), model minus reference: mean and spread over keys."""
    def one(midi):
        rise = lambda source: rms_level(*source(midi, 4)) - rms_level(*source(midi, 1))
        return rise(lambda m, l: render(m, l, 1.5)) - rise(reference)
    errors = np.array(parallel(one, range(24, 106, 6)))
    return float(errors.mean()), float(errors.std())


def onset_features(rate: int, stereo: np.ndarray, midi: int) -> tuple[float, float]:
    """Peak of the first 5 ms vs 10-30 ms, and the share of that attack below f0/2."""
    mono = np.concatenate([np.zeros(rate // 10), stereo.mean(axis=1)])
    start = int(np.argmax(np.abs(mono) > 0.3 * np.abs(mono).max()))
    ms = lambda t: int(t * rate / 1000)
    overshoot = 20 * np.log10(np.abs(mono[start - ms(2):start + ms(3)]).max() /
                              np.abs(mono[start + ms(10):start + ms(30)]).max())
    low = sosfilt(butter(4, fundamental(midi) / 2, fs=rate, output="sos"), mono)[start - ms(2):start + ms(4)]
    attack = mono[start - ms(2):start + ms(4)]
    return overshoot, 10 * np.log10(np.mean(low**2) / np.mean(attack**2))


def onset_error() -> tuple[float, float, float]:
    items = [(m, l) for m in (36, 48, 60, 72, 84, 96, 105) for l in (1, 3, 4)]
    def one(item):
        midi, layer = item
        return onset_features(*render(midi, layer, 0.4), midi), onset_features(*reference(midi, layer), midi)
    pairs = parallel(one, items)
    model = np.array([p[0] for p in pairs])
    ref = np.array([p[1] for p in pairs])
    return float(model[:, 0].mean()), float(model[:, 0].max()), float(np.mean(model[:, 1] - ref[:, 1]))


def brightness_by_layer() -> dict[int, float]:
    """Mean octave-band level 1-8 kHz (dB re total), model minus reference, per layer, over the
    bands where the reference is 6 dB above its noise floor (soft notes' 4-8 kHz often is not)."""
    high = [i for i, lo in enumerate(BANDS[:-1]) if 1000 <= lo < 8000]
    def one(item):
        midi, layer = item
        ref, floor = octave_bands(*reference(midi, layer), floor=True)
        diff = octave_bands(*render(midi, layer, 1.0)) - ref  # the key held through the window, as recorded
        return layer, [diff[i] for i in high if ref[i] > floor[i] + 6]
    results = parallel(one, [(m, l) for m in (36, 48, 60, 72, 84) for l in (1, 2, 3, 4)])
    return {layer: float(np.mean([d for l, v in results if l == layer for d in v])) for layer in (1, 2, 3, 4)}


def attack_share(rate: int, stereo: np.ndarray) -> float:
    """2-8 kHz share of the attack, 2 ms before to 10 ms after onset, dB re the attack's own energy:
    against the whole note, the recordings' room swell would make the dry model look hot."""
    left = stereo[:, 0]
    start = onset(left)
    window = slice(max(start - int(0.002 * rate), 0), start + int(0.01 * rate))
    high = sosfilt(butter(6, [2000, 8000], btype="band", fs=rate, output="sos"), left)
    return 10 * np.log10(np.sum(high[window] ** 2) / np.sum(left[window] ** 2))


def attack_brightness() -> dict[int, float]:
    """Attack 2-8 kHz share, model minus reference, per layer, C2-C6."""
    def one(item):
        midi, layer = item
        return layer, attack_share(*render(midi, layer, 0.4)) - attack_share(*reference(midi, layer))
    results = parallel(one, [(m, l) for m in (36, 48, 60, 72, 84) for l in (1, 2, 3, 4)])
    return {layer: float(np.mean([d for l, d in results if l == layer])) for layer in (1, 2, 3, 4)}


# --- closed-form refits ----------------------------------------------------------------------

def gated_t60(t: np.ndarray, db: np.ndarray, window: np.ndarray, floor: float) -> float | None:
    """T60 from a line fit over the window, or None unless the envelope drops at least 5 dB, stays
    within 3 dB RMS of the line and ends 15 dB above the band's noise floor. Longer T60s than the
    window can show are rejected rather than extrapolated."""
    tw, dw = t[window], db[window]
    if len(tw) < 40:
        return None
    slope, intercept = np.polyfit(tw, dw, 1)
    residual = np.sqrt(np.mean((dw - slope * tw - intercept) ** 2))
    drop = -slope * (tw[-1] - tw[0])
    if drop < 5 or residual > 3 or slope * tw[-1] + intercept - floor < 15:
        return None
    return -60 / slope


def decay_laws() -> tuple[tuple[float, float], tuple[float, float], str]:
    """Fit T60 = a * (f/440)^b to gated early (0.05-0.4 s) and late (2-7 s) slopes, partials 1-3."""
    early, late, tried = [], [], 0
    for midi in FIT_KEYS:
        rate, stereo = reference(midi, 3)
        mono = stereo.mean(axis=1)
        mono = mono[onset(mono):]
        for k in (1, 2, 3):
            if k > len(PARTIALS[midi]) or PARTIALS[midi][k - 1] > 6000:
                continue
            freq = PARTIALS[midi][k - 1]
            sos = butter(2, [freq * 0.994, freq * 1.006], btype="band", fs=rate, output="sos")
            envelope = np.abs(hilbert(sosfiltfilt(sos, mono)))[::240]
            db = 20 * np.log10(envelope / envelope[:60].max() + 1e-12)
            t = np.arange(len(db)) / 200
            # Conservative floor: the band's quietest level in the last second, which may still hold tail.
            floor = np.percentile(db[t > t[-1] - 1.0], 5)
            tried += 1
            for points, window in ((early, (t > 0.05) & (t < 0.4)),
                                   (late, (t > 2.0) & (t < min(len(mono) / rate - 0.5, 7.0)))):
                if (t60 := gated_t60(t, db, window, floor)) is not None:
                    points.append((freq, t60))

    def fit(points):
        x = np.log2(np.array([p[0] for p in points]) / 440)
        y = np.log(np.array([p[1] for p in points]))
        result = least_squares(lambda c: c[0] + c[1] * x - y, [1, 0], loss="soft_l1", f_scale=0.5)
        return float(np.exp(result.x[0])), float(result.x[1] / np.log(2))
    return fit(early), fit(late), f"{len(early)} early and {len(late)} late of {tried} partials accepted"


def level_law_correction(current_knee: float, current_exponent: float) -> tuple[float, float]:
    """Refit the key level law (gain = max(1, f0/knee)^exponent) on layers 1, 3 and 4, fitting keys."""
    fit = np.array([midi - 21 for midi in FIT_KEYS])
    errors = np.mean([key_level_errors(layer) for layer in (1, 3, 4)], axis=0)[fit]
    f0 = np.array([fundamental(m) for m in FIT_KEYS])
    current = 20 * current_exponent * np.log10(np.maximum(1, f0 / current_knee))
    def total(c):
        corrected = errors - current + 20 * c[1] * np.log10(np.maximum(1, f0 / c[0]))
        return corrected - corrected.mean()
    result = least_squares(total, [current_knee, current_exponent], bounds=([20, 0], [1000, 3]))
    return float(result.x[0]), float(result.x[1])


def main() -> None:
    if not RENDERER.is_file():
        sys.exit("Build the renderer first: cmake --build build")
    print(f"Renderer: {RENDERER}")
    print(f"spectrum error, fitting layers 1/3/4: {spectrum_error([1, 3, 4]):5.2f} dB")
    print(f"spectrum error, held-out layer 2:     {spectrum_error([2]):5.2f} dB")
    levels = dict(zip(range(21, 109), key_level_errors(3)))
    print(f"key level error std, layer 3:         {np.std(list(levels.values())):5.2f} dB "
          f"(fitting keys {np.std([levels[m] for m in FIT_KEYS]):4.2f}, "
          f"split keys {np.std([levels[m] for m in HELD_OUT_KEYS]):4.2f}); worst "
          + ", ".join(f"{note_name(m)} {levels[m]:+.1f}" for m in sorted(levels, key=lambda m: -abs(levels[m]))[:3]))
    sustain = sustain_errors()
    print(f"sustain error, layer 3:               {np.sqrt(np.mean(np.square(list(sustain.values())))):5.2f} dB; worst "
          + ", ".join(f"{note_name(m)} {sustain[m]:.1f}" for m in sorted(sustain, key=lambda m: -sustain[m])[:3]))
    model, ref = correlations(lambda m: render(m, 3, 2.0, pedal=1)), correlations(lambda m: reference(m, 3))
    print(f"L/R correlation mean/median:          model {model.mean():5.2f}/{np.median(model):5.2f}  "
          f"reference {ref.mean():5.2f}/{np.median(ref):5.2f}")
    halo_diff = halo_error()
    print("halo plausibility (not calibration):  "
          + " ".join(f"{v:+5.1f}" for v in halo_diff.mean(axis=0)) + " dB (0.3-0.6, 0.6-1.2, 1.2-2.4 s); "
          f"per-note spread {np.std(halo_diff.mean(axis=1)):4.1f} dB")
    print(f"knock, between-partial 20-100 ms: {knock_error():+5.1f} dB")
    print(f"phantom partials, A0-A1, mf/ff:  {phantom_error():+5.1f} dB")
    span, spread = velocity_span()
    print(f"velocity span L1-L4, model - ref: {span:+5.1f} dB (per-key spread {spread:3.1f} dB)")
    mean, worst, low = onset_error()
    print(f"onset overshoot mean/max:             {mean:+5.1f}/{worst:+5.1f} dB; low-frequency share vs ref {low:+5.1f} dB")
    bright = brightness_by_layer()
    print("1-8 kHz brightness, model minus ref:  " + "  ".join(f"L{l} {v:+5.1f}" for l, v in bright.items()) + " dB")
    attack = attack_brightness()
    print("attack 2-8 kHz share, model - ref:    " + "  ".join(f"L{l} {v:+5.1f}" for l, v in attack.items()) + " dB")
    if "--refit" in sys.argv:
        (early_a, early_b), (late_a, late_b), counts = decay_laws()
        print(f"decay fits: {counts}")
        print(f"refit prompt decay:     T60 = {early_a:.2f} s * (f/440)^{early_b:.3f}   (code: 4.89, -0.745)")
        print(f"refit aftersound decay: T60 = {late_a:.2f} s * (f/440)^{late_b:.3f}   (code: 16.87, -0.444)")
        knee, exponent = level_law_correction(302.3, 1.042)
        print(f"refit key level law:    knee {knee:.1f} Hz, exponent {exponent:.3f}   (code: 302.3, 1.042; "
              "one-step linear correction, not a fixed point: knock and sympathy don't scale with the law)")


if __name__ == "__main__":
    main()
