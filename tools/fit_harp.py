#!/usr/bin/env python3
"""Fit PianoCraft Harp's decay law to VCSL's concert harp (CC0; model/vcsl-provenance.json).

    .venv/bin/python tools/fit_harp.py

Reads calibration/vcsl/harp/KSHarp_*.wav (one take per note, E1-F7, mf and f; the names are the sounding pitch).
Each note's partials are followed up its spectrum (0.05-1.05 s) from the fundamental, and each partial's decay is a
line through its level (dB, a 4-period window) over 0.2-2 s (to 1 s from C4 up) where it stands 15 dB over the
file's end. Fits T60 = A (f / 440)^p 2^(-q (key - 60) / 12) to the rates (log error, soft L1), and checks it on the
odd-numbered keys after fitting the even ones. Prints the constants for HarpEngine.cpp's partial().

Partial levels are measured too but not fitted: single takes through one room colour each note on its own, and laws
fitted to them improved the fitting notes (16.6 to 12 dB rms) but not the held-out ones (17.7 to 16).
"""
from __future__ import annotations

import re
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.optimize import least_squares

ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / "calibration" / "vcsl" / "harp"
NAMES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def load(path):
    rate, x = wavfile.read(path)
    x = x.astype(float) / {np.dtype("int16"): 2 ** 15, np.dtype("int32"): 2 ** 31}.get(x.dtype, 1)
    mono = x.mean(axis=1) if x.ndim > 1 else x
    onset = int(np.argmax(np.abs(mono) > 0.1 * np.abs(mono).max()))
    return rate, mono[max(onset - int(0.002 * rate), 0):]


def series(rate, x, f0, top=25):
    seg = x[int(0.05 * rate):int(1.05 * rate)]
    n = 1 << 19
    s = 10 * np.log10(np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n)) ** 2 + 1e-30)
    f = np.fft.rfftfreq(n, 1 / rate)
    near = (f > 0.95 * f0) & (f < 1.05 * f0)
    found = {1: f[near][np.argmax(s[near])]}
    f1 = found[1]
    for k in range(2, top + 1):
        if k * f1 > min(14000, 0.45 * rate):
            break
        a, b = sorted(found)[-2:] if len(found) >= 2 else (None, 1)
        guess = k * f1 if a is None else found[b] + (k - b) * (found[b] - found[a]) / (b - a)
        span = np.abs(f - guess) < 0.12 * f1
        i = np.argmax(s[span])
        if s[span][i] > np.median(s[span]) + 15:
            found[k] = f[span][i]
    return found


def levels(rate, x, freqs, times):
    n = int(max(0.03, 4 / freqs[0]) * rate)
    w = np.hanning(n)
    basis = np.exp(-2j * np.pi * np.outer(np.arange(n) / rate, freqs)) * w[:, None] * (2 / w.sum())
    starts = [min(max(int(t * rate) - n // 2, 0), len(x) - n) for t in times]
    return np.array([20 * np.log10(np.abs(x[s:s + n] @ basis) + 1e-12) for s in starts])


def measure():
    rows = []  # key, partial frequency (Hz), decay (dB/s)
    for path in sorted(CORPUS.glob("KSHarp_*.wav")):
        name = re.search(r"KSHarp_([A-G])(\d)_", path.name)
        key = 12 * (int(name.group(2)) + 1) + NAMES[name.group(1)]
        rate, x = load(path)
        found = series(rate, x, 440 * 2 ** ((key - 69) / 12))
        freqs = np.array([found[k] for k in sorted(found)])
        times = np.arange(0.2, min(2.0 if key < 60 else 1.0, len(x) / rate - 0.1), 0.05)
        track = levels(rate, x, freqs, times)
        floor = levels(rate, x, freqs, [len(x) / rate - 0.1])[0]
        for i, f in enumerate(freqs):
            held = track[:, i] > max(floor[i], track[:, i].max() - 60) + 15
            if held.sum() >= 6:
                rate_db = np.polyfit(times[held], track[held, i], 1)[0]
                if rate_db < 0:
                    rows.append((key, f, rate_db))
    return np.array(rows)


def law(p, f, key):
    a, power, q = p
    return -60 / (a * (f / 440) ** power) * 2 ** (q * (key - 60) / 12)


def main():
    rows = measure()
    key, f, rate = rows.T
    error = lambda p, sel: np.log2(law(p, f[sel], key[sel]) / rate[sel])
    every = np.ones(len(key), bool)
    even = key % 2 == 0
    fit = least_squares(lambda p: error(p, every), [5.0, -0.7, 0.0], loss="soft_l1").x
    half = least_squares(lambda p: error(p, even), [5.0, -0.7, 0.0], loss="soft_l1").x
    eta = lambda f_, k_: -60 * (1 / (5 * (f_ / 440) ** -0.7) + np.pi * f_ * 2e-3 * np.clip((60 - k_) / 20, 0, 1) / np.log(1000))
    old = np.log2(eta(f, key) / rate)
    print(f"{len(rows)} partials' decays from {len(set(key))} keys (mf and f)")
    print(f"T60 = {fit[0]:.2f} s (f / 440)^{fit[1]:.3f} 2^(-{fit[2]:.3f} (key - 60) / 12): "
          f"mean |log2 error| {np.mean(np.abs(error(fit, every))):.2f} (the old law with wire loss: {np.mean(np.abs(old)):.2f})")
    print(f"fitted on even keys {np.round(half, 3)}: odd keys {np.mean(np.abs(error(half, ~even))):.2f}, "
          f"even {np.mean(np.abs(error(half, even))):.2f}")


if __name__ == "__main__":
    main()
