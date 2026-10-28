#!/usr/bin/env python3
"""Score the current renderer against the Meta Piano corpus and re-derive the fitted model laws.

Layers 1, 3 and 4 are the fitting layers; layer 2 is reported separately as held-out data and is
never used to choose a value. See docs/model-laws.md for what each metric guards.

    .venv/bin/python tools/evaluate_model.py            # metrics
    .venv/bin/python tools/evaluate_model.py --refit    # metrics plus closed-form law refits

Set PIANOCRAFT_RENDERER to score another renderer build.
"""

from __future__ import annotations

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


def octave_bands(rate: int, stereo: np.ndarray) -> np.ndarray:
    left = stereo[:, 0]
    start = onset(left)
    window = left[start + int(0.01 * rate):start + int(0.4 * rate)]
    power = np.abs(np.fft.rfft(window * np.hanning(len(window)))) ** 2
    freq = np.fft.rfftfreq(len(window), 1 / rate)
    return np.array([10 * np.log10(power[(freq >= lo) & (freq < hi)].sum() / power.sum() + 1e-12)
                     for lo, hi in zip(BANDS[:-1], BANDS[1:])])


def spectrum_error(layers: list[int]) -> float:
    """Octave-band shape (left channel, 10-400 ms) vs reference; below f0 only excess counts."""
    def one(item):
        midi, layer = item
        model = octave_bands(*render(midi, layer, 1.0))
        diff = model - octave_bands(*reference(midi, layer))
        above = np.array([hi > fundamental(midi) for hi in BANDS[1:]])
        error = np.where(above, diff, np.maximum(diff, 0.0))
        return error[model > -90]
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


def sustain_error() -> float:
    """RMS level at 0.25-5 s relative to the first 100 ms, pedal held, layer 3."""
    times = (0.25, 0.5, 1, 2, 3, 4, 5)

    def curve(rate, stereo):
        mono = stereo.mean(axis=1)
        mono = mono[onset(mono):]
        width = int(0.1 * rate)
        peak = np.sqrt(np.mean(mono[:width] ** 2))
        return np.array([20 * np.log10(np.sqrt(np.mean(mono[int(t * rate):int(t * rate) + width] ** 2)) / peak + 1e-12)
                         if int(t * rate) + width < len(mono) else np.nan for t in times])

    def one(midi):
        return curve(*render(midi, 3, 6.0, pedal=1, sympathy=0.0)) - curve(*reference(midi, 3))
    diff = np.concatenate(parallel(one, range(24, 109, 4)))
    diff = np.clip(diff[np.isfinite(diff)], -30, 30)
    return float(np.sqrt(np.mean(diff**2)))


def correlations(source) -> np.ndarray:
    def one(midi):
        rate, stereo = source(midi)
        start = int(np.argmax(np.abs(stereo).max(axis=1) > 0.05 * np.abs(stereo).max()))
        window = stereo[start + int(0.05 * rate):start + int(1.5 * rate)]
        return np.corrcoef(window[:, 0], window[:, 1])[0, 1]
    return np.array(parallel(one, range(24, 106, 3)))


def halo(rate: int, stereo: np.ndarray, midi: int) -> np.ndarray:
    """Energy between the note's own partials (excluding mains hum), 0.3-2.4 s, dB re band total."""
    mono = stereo.mean(axis=1)
    mono = mono[onset(mono):]
    result = []
    for start, stop in ((0.3, 0.6), (0.6, 1.2), (1.2, 2.4)):
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
    notes = (36, 48, 57, 64, 69, 76, 84, 91)
    def one(midi):
        return halo(*render(midi, 3, 3.0, pedal=1), midi) - halo(*reference(midi, 3), midi)
    return np.array(parallel(one, notes))


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
    """Mean octave-band level 1-8 kHz (dB re total), model minus reference, per layer."""
    high = [i for i, lo in enumerate(BANDS[:-1]) if 1000 <= lo < 8000]
    def one(item):
        midi, layer = item
        diff = octave_bands(*render(midi, layer, 0.6)) - octave_bands(*reference(midi, layer))
        return layer, float(np.mean(diff[high]))
    results = parallel(one, [(m, l) for m in (36, 48, 60, 72, 84) for l in (1, 2, 3, 4)])
    return {layer: float(np.mean([v for l, v in results if l == layer])) for layer in (1, 2, 3, 4)}


# --- closed-form refits ----------------------------------------------------------------------

def decay_laws() -> tuple[tuple[float, float], tuple[float, float]]:
    """Fit T60 = a * (f/440)^b to early (0.05-0.4 s) and late (2-7 s) envelope slopes, partials 1-3."""
    early, late = [], []
    for midi in range(21, 109):
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
            late_window = (t > 2.0) & (t < min(len(mono) / rate - 0.5, 7.0))
            early_window = (t > 0.05) & (t < 0.4)
            if late_window.sum() < 100:
                continue
            slope_late = np.polyfit(t[late_window], db[late_window], 1)[0]
            slope_early = np.polyfit(t[early_window], db[early_window], 1)[0]
            if slope_early < 0 and 0.3 < -60 / slope_early < 60:
                early.append((freq, -60 / slope_early))
            if slope_late < 0 and 1 < -60 / slope_late < 80:
                late.append((freq, -60 / slope_late))

    def fit(points):
        x = np.log2(np.array([p[0] for p in points]) / 440)
        y = np.log(np.array([p[1] for p in points]))
        result = least_squares(lambda c: c[0] + c[1] * x - y, [1, 0], loss="soft_l1", f_scale=0.5)
        return float(np.exp(result.x[0])), float(result.x[1] / np.log(2))
    return fit(early), fit(late)


def level_law_correction(current_knee: float, current_exponent: float) -> tuple[float, float]:
    """Refit the key level law (gain = max(1, f0/knee)^exponent) on layers 1, 3 and 4."""
    errors = np.mean([key_level_errors(layer) for layer in (1, 3, 4)], axis=0)
    f0 = np.array([fundamental(m) for m in range(21, 109)])
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
    print(f"key level error std, layer 3:         {np.std(key_level_errors(3)):5.2f} dB")
    print(f"sustain error, layer 3:               {sustain_error():5.2f} dB")
    model, ref = correlations(lambda m: render(m, 3, 2.0, pedal=1)), correlations(lambda m: reference(m, 3))
    print(f"L/R correlation mean/median:          model {model.mean():5.2f}/{np.median(model):5.2f}  "
          f"reference {ref.mean():5.2f}/{np.median(ref):5.2f}")
    halo_diff = halo_error()
    print("pedal-down halo, model minus ref:     "
          + " ".join(f"{v:+5.1f}" for v in halo_diff.mean(axis=0)) + " dB (0.3-0.6, 0.6-1.2, 1.2-2.4 s); "
          f"per-note spread {np.std(halo_diff.mean(axis=1)):4.1f} dB")
    mean, worst, low = onset_error()
    print(f"onset overshoot mean/max:             {mean:+5.1f}/{worst:+5.1f} dB; low-frequency share vs ref {low:+5.1f} dB")
    bright = brightness_by_layer()
    print("1-8 kHz brightness, model minus ref:  " + "  ".join(f"L{l} {v:+5.1f}" for l, v in bright.items()) + " dB")
    if "--refit" in sys.argv:
        (early_a, early_b), (late_a, late_b) = decay_laws()
        print(f"refit prompt decay:     T60 = {early_a:.2f} s * (f/440)^{early_b:.3f}   (code: 4.89, -0.745)")
        print(f"refit aftersound decay: T60 = {late_a:.2f} s * (f/440)^{late_b:.3f}   (code: 16.87, -0.444)")
        knee, exponent = level_law_correction(64.4, 1.505)
        print(f"refit key level law:    knee {knee:.1f} Hz, exponent {exponent:.3f}   (code: 64.4, 1.505)")


if __name__ == "__main__":
    main()
