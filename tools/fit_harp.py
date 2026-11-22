#!/usr/bin/env python3
"""Fit PianoCraft Harp's decay law to VCSL's concert harp (CC0; model/vcsl-provenance.json).

    .venv/bin/python tools/fit_harp.py            the decay law
    .venv/bin/python tools/fit_harp.py levels     the pluck position and the partials' levels: writes HarpCalibration.hpp
    .venv/bin/python tools/fit_harp.py rounds [n] n rounds (default 4): render every take, refine the radiation
    .venv/bin/python tools/fit_harp.py score      render every take: partial levels, key levels, the rise, the noise

Reads calibration/vcsl/harp/KSHarp_*.wav (one take per note, E1-F7, mf and f; the names are the sounding pitch).
Each note's partials are followed up its spectrum (0.05-1.05 s) from the fundamental, and each partial's decay is a
line through its level (dB, a 4-period window) over 0.2-2 s (to 1 s from C4 up) where it stands 15 dB over the
file's end. Fits T60 = A (f / 440)^p 2^(-q (key - 60) / 12) to the rates (log error, soft L1), and checks it on the
odd-numbered keys after fitting the even ones. Prints the constants for HarpEngine.cpp's partial().

levels: each note's partials 1-16 at 0.05-0.3 s (the strongest within 60 dB of each note's), first as a comb,
20 log10|sin(n pi beta)| over a smooth trend, for each take's pluck point; then, beta fixed, as
20 log10|sin(n pi beta)| + slope(key) log2 n + R(f) + c(note): the pluck's fall in partial number, linear in key, and
the radiation (box, board and microphones), piecewise-linear in log2 f on half-octave knots with a second-difference
smoothing, the same for every note; c takes each take's level. Fitted on even keys (soft L1), scored on odd ones
against the engine's old law. Single takes colour each note with the room on its own: per-note levels don't carry
(fitted to them, held-out notes improved only 17.7 to 16 dB), smooth laws through all of them do.

score: every take rendered at its dynamic's velocity against its recording. Partial levels as above. Key levels: each
take's 50-150 ms level, each dynamic's set about its mean, and a quadratic in octaves from C4 fitted on the even keys
through what remains (zero when HarpEngine.cpp's stringLevelDb() holds; add what it prints there). The rise: each
take's level per millisecond over its first 50 ms, re its 50-150 ms level. The noise, from G4 up, where the partials
stand far enough apart for 20 ms windows: the power between them (density x octave band) re the partials' power at
50-150 ms, per octave band and window start; HarpEngine.cpp's pluck noise (level, corners, decay) and onset are
selected against these.
"""
from __future__ import annotations

import re
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.optimize import least_squares

ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / "calibration" / "vcsl" / "harp"
HEADER = ROOT / "src" / "engine" / "HarpCalibration.hpp"
VOICE = ROOT / "build" / "pianocraft-render-voice"
VELOCITY = {"p": 0.35, "mp": 0.45, "mf": 0.55, "f": 0.8}  # the takes' dynamics as the engine's velocity
KNOTS = np.arange(np.log2(30.0), np.log2(14000.0) + 0.5, 0.5)  # the radiation's, half an octave apart
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


def partial_levels(rate, x, f0, top=16):
    seg = x[int(0.05 * rate):int(0.30 * rate)]
    s = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), 1 << 18)) ** 2
    f = np.fft.rfftfreq(1 << 18, 1 / rate)
    out = np.full(top, np.nan)
    for k in range(1, top + 1):
        near = np.abs(f - k * f0) < 0.06 * f0 * np.sqrt(k)
        if k * f0 < min(14000.0, 0.45 * rate) and near.any():
            out[k - 1] = 10 * np.log10(s[near].max() + 1e-30)
    return out


def takes():
    for path in sorted(CORPUS.glob("KSHarp_*.wav")):
        name = re.search(r"KSHarp_([A-G])(\d)_([a-z]+)", path.name)
        key = 12 * (int(name.group(2)) + 1) + NAMES[name.group(1)]
        rate, x = load(path)
        f0 = 440 * 2 ** ((key - 69) / 12) * 2 ** (-10 / 1200)  # the instrument sits ~10 cents flat
        yield key, name.group(3), f0, partial_levels(rate, x, f0)


def hat(lf):
    row = np.zeros(len(KNOTS))
    i = int(np.clip(np.searchsorted(KNOTS, lf) - 1, 0, len(KNOTS) - 2))
    t = float(np.clip((lf - KNOTS[i]) / 0.5, 0, 1))
    row[i], row[i + 1] = 1 - t, t
    return row


def usable(level):
    return [n for n in range(1, 17) if np.isfinite(level[n - 1]) and level[n - 1] > np.nanmax(level) - 60]


def take_error(recorded, modelled):
    """A take's partial levels against the model's, dB rms, each matched in level (the median difference)."""
    ns = [n for n in usable(recorded) if np.isfinite(modelled[n - 1])]
    e = np.array([recorded[n - 1] - modelled[n - 1] for n in ns])
    return float(np.sqrt(np.mean(np.minimum(np.abs(e - np.median(e)), 30) ** 2)))


def write_header(radiation, slope, note):
    HEADER.write_text(f"""#pragma once

// Generated by tools/fit_harp.py ({note}) from VCSL's concert harp (model/vcsl-provenance.json). Do not edit.
#include <array>

namespace pianocraft {{

// What the board, box and microphones make of each frequency on top of the engine's own soundbox, dB on half-octave
// knots from 30 Hz, and how a pluck's partials fall in partial number, dB per octave.
inline constexpr std::array<double, {len(radiation)}> harpRadiationDb{{
    {", ".join(f"{v:.1f}" for v in radiation)}}};
inline constexpr double harpPluckSlopeDb = {slope:.2f};

}} // namespace pianocraft
""")


def read_header():
    text = HEADER.read_text()
    radiation = np.array([float(v) for v in re.search(r"harpRadiationDb\{\s*([^}]*)\}", text).group(1).split(",")])
    return radiation, float(re.search(r"harpPluckSlopeDb = (-?[\d.]+)", text).group(1))


def render(item):
    key, dyn = item
    out = Path(tempfile.gettempdir()) / f"pianocraft-harp-{key}-{dyn}.wav"
    # Pluck Noise (12) at 1, the recordings' level; the plugin defaults to 0.06
    subprocess.run([str(VOICE), "harp", str(out), str(key), str(VELOCITY[dyn]), "1.0", "0.9", "12=1"], check=True, capture_output=True)
    return load(out)


def render_take(item):
    key, dyn, f0 = item
    return partial_levels(*render((key, dyn)), f0)


def body_level(rate, x):
    return 10 * np.log10(np.mean(x[int(0.05 * rate):int(0.15 * rate)] ** 2))


def rise(rate, x, ms=(1, 2, 3, 5, 8, 12, 20, 30, 50)):
    start = max(int(np.argmax(np.abs(x) > 0.05 * np.abs(x).max())) - rate // 1000, 0)
    p = x[start:start + 51 * (rate // 1000)] ** 2
    per_ms = 10 * np.log10(p.reshape(51, -1).mean(1) + 1e-30)
    return np.array([per_ms[m] for m in ms]) - body_level(rate, x)


NOISE_BANDS = [(250, 500), (500, 1000), (1000, 2000), (2000, 4000), (4000, 8000), (8000, 16000)]
NOISE_STARTS = [0.0, 0.01, 0.02, 0.04, 0.08, 0.16]


def noise(rate, x, f0):
    body = x[int(0.05 * rate):int(0.15 * rate)]
    s = np.abs(np.fft.rfft(body * np.hanning(len(body)), 1 << 16)) ** 2 / np.sum(np.hanning(len(body)) ** 2)
    f = np.fft.rfftfreq(1 << 16, 1 / rate)
    partials = s[np.min(np.abs(f[:, None] - f0 * np.arange(1, 30)[None, :]), 1) < 0.12 * f0].sum()
    n, w = int(0.02 * rate), np.hanning(int(0.02 * rate))
    fr = np.fft.rfftfreq(1 << 14, 1 / rate)
    away = np.min(np.abs(fr[:, None] - f0 * np.arange(1, 60)[None, :]), 1) > 0.3 * f0
    out = np.full((len(NOISE_BANDS), len(NOISE_STARTS)), np.nan)
    for j, t in enumerate(NOISE_STARTS):
        power = np.abs(np.fft.rfft(x[int(t * rate):int(t * rate) + n] * w, 1 << 14)) ** 2 / np.sum(w ** 2)
        for i, (lo, hi) in enumerate(NOISE_BANDS):
            band = (fr >= lo) & (fr < hi)
            if (away & band).sum() > 3 and hi < 0.48 * rate:
                out[i, j] = 10 * np.log10(power[away & band].mean() * band.sum() / partials)
    return out


def score():
    subprocess.run(["cmake", "--build", str(ROOT / "build"), "--target", "pianocraft-render-voice"], check=True, capture_output=True)
    paths = sorted(CORPUS.glob("KSHarp_*.wav"))
    items = []
    for path in paths:
        name = re.search(r"KSHarp_([A-G])(\d)_([a-z]+)", path.name)
        items.append((12 * (int(name.group(2)) + 1) + NAMES[name.group(1)], name.group(3)))
    with ThreadPoolExecutor(8) as pool:
        modelled = list(pool.map(render, items))
    recorded = [load(path) for path in paths]
    f0s = [440 * 2 ** ((key - 69) / 12) for key, _ in items]
    keys = np.array([key for key, _ in items])
    registers = (("bass", keys < 48), ("middle", (keys >= 48) & (keys < 72)), ("treble", keys >= 72))
    flat = 2 ** (-10 / 1200)  # the recordings sit ~10 cents flat
    errors = np.array([take_error(partial_levels(*r, f0 * flat), partial_levels(*m, f0)) for r, m, f0 in zip(recorded, modelled, f0s)])
    print("partial levels, dB rms per partial: " + ", ".join(f"{n} {errors[sel].mean():.1f}" for n, sel in registers) +
          f"; even keys {errors[keys % 2 == 0].mean():.1f}, odd {errors[keys % 2 == 1].mean():.1f}")
    level = np.array([body_level(*r) - body_level(*m) for r, m in zip(recorded, modelled)])
    loud = np.array([dyn == "f" for _, dyn in items])
    for sel in (loud, ~loud):
        level[sel] -= level[sel].mean()
    octaves = (keys - 60) / 12
    even = keys % 2 == 0
    law = np.linalg.lstsq(np.stack([octaves, octaves ** 2, loud, ~loud], 1)[even].astype(float), level[even], rcond=None)[0]
    print(f"key levels, recording - model: spread {np.std(level):.1f} dB (odd keys {np.std(level[~even]):.1f}); "
          f"what remains, as stringLevelDb's: {law[0]:+.2f} octaves {law[1]:+.2f} octaves^2")
    gap = np.array([rise(*m) - rise(*r) for r, m in zip(recorded, modelled)])
    print("the rise, model - recording, dB at 1/2/3/5/8/12/20/30/50 ms; rms:")
    for n, sel in registers:
        print(f"  {n:7s}" + "".join(f"{v:+5.0f}" for v in gap[sel].mean(0)) + f"   {np.sqrt(np.mean(gap[sel] ** 2)):.1f}")
    high = keys >= 67
    rec = np.nanmean([noise(*r, f0 * flat) for r, f0, h in zip(recorded, f0s, high) if h], 0)
    mod = np.nanmean([noise(*m, f0) for m, f0, h in zip(modelled, f0s, high) if h], 0)
    print(f"the noise between partials ({high.sum()} takes from G4), dB re the partials at 50-150 ms, recording | model, "
          "windows from " + "/".join(f"{1000 * t:.0f}" for t in NOISE_STARTS) + " ms:")
    for (lo, hi), a, b in zip(NOISE_BANDS, rec, mod):
        print(f"  {lo:5d}-{hi:5d} Hz" + "".join(f"{v:+5.0f}" for v in a) + "  |" + "".join(f"{v:+5.0f}" for v in b))


def fit_rounds(rounds):
    """Render every take through the engine (soundbox, second polarization, sympathetic strings and all) and move the
    radiation by a smooth fit (as fit_levels', on even keys) through what each take's partials, matched in level,
    still need. The engine's soundbox stays for its ringing; this gives the whole its recorded spectrum."""
    notes = list(takes())
    radiation, slope = read_header()
    for round_ in range(rounds + 1):
        subprocess.run(["cmake", "--build", str(ROOT / "build"), "--target", "pianocraft-render-voice"], check=True, capture_output=True)
        with ThreadPoolExecutor(8) as pool:
            modelled = list(pool.map(render_take, [(k, d, f0) for k, d, f0, _ in notes]))
        even = [take_error(r, m) for (k, _, _, r), m in zip(notes, modelled) if k % 2 == 0]
        odd = [take_error(r, m) for (k, _, _, r), m in zip(notes, modelled) if k % 2 == 1]
        print(f"round {round_}: rendered partial levels, dB rms per partial, even keys {np.mean(even):.1f}, odd (held out) {np.mean(odd):.1f}")
        if round_ == rounds:
            break
        rows, ys = [], []
        for (key, _, f0, recorded), model in zip(notes, modelled):
            ns = [n for n in usable(recorded) if np.isfinite(model[n - 1])]
            if key % 2 or not ns:
                continue
            e = np.array([recorded[n - 1] - model[n - 1] for n in ns])
            for n, v in zip(ns, e - np.median(e)):
                rows.append(hat(np.log2(n * f0)))
                ys.append(np.clip(v, -30, 30))
        x, y = np.array(rows), np.array(ys)
        smooth = np.zeros((len(KNOTS) - 2, len(KNOTS)))
        for i in range(len(KNOTS) - 2):
            smooth[i, i:i + 3] = [3.0, -6.0, 3.0]
        w = np.ones(len(y))
        for _ in range(8):
            step = np.linalg.lstsq(np.vstack([x * w[:, None], smooth]), np.concatenate([y * w, np.zeros(len(smooth))]), rcond=None)[0]
            w = 1 / np.sqrt(1 + ((y - x @ step) / 4) ** 2)
        radiation = radiation + step
        radiation -= radiation.max()
        write_header(radiation, slope, f"levels and {round_ + 1} rendered rounds")


def fit_levels():
    notes = list(takes())
    grid, betas = np.arange(0.10, 0.5001, 0.005), []
    for key, _, f0, level in notes:
        n = np.arange(1, 17)
        ok = np.isfinite(level) & (level > np.nanmax(level) - 60)
        if ok.sum() < 6:
            continue
        trend = np.stack([np.ones(ok.sum()), np.log2(n[ok]), np.log2(n[ok]) ** 2], 1)
        def error(b):
            y = level[ok] - 20 * np.log10(np.abs(np.sin(n[ok] * np.pi * b)) + 1e-3)
            return np.sqrt(np.mean(np.minimum(np.abs(y - trend @ np.linalg.lstsq(trend, y, rcond=None)[0]), 15) ** 2))
        betas.append(grid[np.argmin([error(b) for b in grid])])
    print(f"pluck point per take: median {np.median(betas):.3f}, quartiles {np.percentile(betas, 25):.3f}-{np.percentile(betas, 75):.3f}")
    beta = float(np.round(np.median(betas) * 40) / 40)  # to 0.025
    knots = KNOTS

    def comb(n):
        return 20 * np.log10(abs(np.sin(n * np.pi * beta)) + 1e-3)

    def rows(select):
        out, ys = [], []
        for j, (key, _, f0, level) in enumerate(notes):
            if not select(key):
                continue
            for n in range(1, 17):
                if np.isfinite(level[n - 1]) and level[n - 1] > np.nanmax(level) - 60:
                    c = np.zeros(len(notes))
                    c[j] = 1
                    out.append(np.concatenate([hat(np.log2(n * f0)), [np.log2(n), np.log2(n) * (key - 60) / 12], c]))
                    ys.append(level[n - 1] - comb(n))
        return np.array(out), np.array(ys)

    x, y = rows(lambda k: k % 2 == 0)
    smooth = np.zeros((len(knots) - 2, x.shape[1]))
    for i in range(len(knots) - 2):
        smooth[i, i:i + 3] = [3.0, -6.0, 3.0]
    w = np.ones(len(y))
    for _ in range(8):  # soft L1, reweighted
        p = np.linalg.lstsq(np.vstack([x * w[:, None], smooth]), np.concatenate([y * w, np.zeros(len(smooth))]), rcond=None)[0]
        w = 1 / np.sqrt(1 + ((y - x @ p) / 4) ** 2)
    radiation, slope = p[:len(knots)] - p[:len(knots)].max(), p[len(knots):len(knots) + 2]

    def fitted(key, f0, n):
        return comb(n) + (slope[0] + slope[1] * (key - 60) / 12) * np.log2(n) + hat(np.log2(n * f0)) @ radiation

    def old(key, f0, n, loud):  # HarpEngine before: pluck at 0.35, 1/n, the fingertip's 12 dB/octave, a by-ear radiation
        f, corner = n * f0, 3000 * (0.55 + 0.65 * (0.8 if loud else 0.55))
        board = (f / 120) / np.sqrt(1 + (f / 120) ** 2) / np.sqrt(1 + (f / 6000) ** 2)
        return 20 * np.log10(abs(np.sin(n * np.pi * 0.35)) / n / (1 + (f / corner) ** 2) * board + 1e-9)

    def score(select, law):
        errors = []
        for key, dyn, f0, level in notes:
            ns = [n for n in range(1, 17) if select(key) and np.isfinite(level[n - 1]) and level[n - 1] > np.nanmax(level) - 60]
            if ns:
                e = np.array([level[n - 1] - law(key, f0, n, dyn == "f") for n in ns])
                errors.append(np.sqrt(np.mean(np.minimum(np.abs(e - np.median(e)), 30) ** 2)))
        return np.mean(errors)
    print(f"beta {beta}: partial levels, dB rms per partial (each take level-matched): fitted law even keys "
          f"{score(lambda k: k % 2 == 0, lambda k, f, n, l: fitted(k, f, n)):.1f}, odd (held out) "
          f"{score(lambda k: k % 2 == 1, lambda k, f, n, l: fitted(k, f, n)):.1f}; the old law odd {score(lambda k: k % 2 == 1, old):.1f}")
    print(f"pluck spectrum {slope[0]:+.2f} dB per octave of partial number, {slope[1]:+.2f} more per octave of key")
    print("radiation, dB on half-octave knots from 30 Hz: {" + ", ".join(f"{v:.1f}" for v in radiation) + "}")
    write_header(radiation, float(slope[0]), "levels")


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
    import sys
    if sys.argv[1:2] == ["levels"]:
        fit_levels()
    elif sys.argv[1:2] == ["score"]:
        score()
    elif sys.argv[1:2] == ["rounds"]:
        fit_rounds(int(sys.argv[2]) if len(sys.argv) > 2 else 4)
    else:
        main()
