#!/usr/bin/env python3
"""Fit the upright's spectrum to the Meta Piano corpus: each key's strike position, the radiation (board and
microphones) and how the hammer brightens with velocity. Writes src/model/SpectrumCalibration.hpp.

    .venv/bin/python tools/fit_piano.py neutral       the analytic strike law and flat tables
    .venv/bin/python tools/fit_piano.py strike        fit each key's strike position from its recordings' notches
    .venv/bin/python tools/fit_piano.py decays        fit each partial's two-stage decay to its recorded envelope
    .venv/bin/python tools/fit_piano.py spectrum [n]  n rounds (default 3): render, compare, refine the level tables
    .venv/bin/python tools/fit_piano.py score         each partial's level error, fitting and held-out

Each partial is compared by its level 0.1 s after the strike (a line fitted to its level over 0.05-0.4 s, both
channels' power). Fits use velocity layers 1, 3 and 4 and the keys outside model/provenance.json's split, as
tools/evaluate_model.py does; layer 2 and the split keys are the check. A partial counts while the recording holds
it 10 dB over its own floor. See docs/model-laws.md.
"""
from __future__ import annotations

import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
import scipy.sparse as sparse
from scipy.sparse.linalg import lsqr

sys.path.insert(0, str(Path(__file__).resolve().parent))
import evaluate_model as ev  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "model" / "SpectrumCalibration.hpp"
CACHE = ROOT / "calibration" / "spectrum-reference.npz"
KEYS = list(range(21, 109))
FIT_LAYERS = (1, 4)  # layer 3 is the reference (0 dB); layer 2 is held out
TOP = 9000.0         # highest partial compared and synthesized
LOWEST, BANDS_PER_OCTAVE, BANDS = 25.0, 12, 110  # the tables' frequencies: 25 Hz up, 1/12 octave
PARTIALS = 112
SPREADS = np.array([0.0005, 0.001, 0.002, 0.003, 0.004, 0.005, 0.006, 0.008, 0.01])


def calibration():
    blocks = ev.HEADER.read_text().split("CalibrationData{")[1:]
    out = {}
    for midi, block in zip(KEYS, blocks):
        f1, b = (float(v) for v in block.split(",")[:2])
        partials = {int(k): float(f) for k, f in re.findall(r"PartialCalibration\{(\d+), ([\d.e-]+)", block) if int(k) > 0}
        out[midi] = (f1, b, partials)
    return out


CAL = calibration()


def partials(midi):
    """The model's partial series: the calibrated partials, then the inharmonic law to TOP (InstrumentModel)."""
    f1, b, calibrated = CAL[midi]
    series = {k: f for k, f in calibrated.items() if 0 < f <= TOP}
    k = max(calibrated) + 1
    while len(series) < 112:
        f = k * f1 * np.sqrt((1 + b * k * k) / (1 + b))
        if f > TOP:
            break
        series[k] = f
        k += 1
    ks = np.array(sorted(series))
    return ks, np.array([series[k] for k in ks])


def measure(rate, stereo, freqs, f1, until=None):
    """Each partial's level (dB, both channels' power) at 0.1 s and its early decay (dB/s), from a line over
    0.05-0.4 s, and its late decay, a line over 1 s to until, with that line's level at 1 s re the level at 0.1 s
    (the knee, where a unison's aftersound takes over). For a recording (until None) also its floor, the 5th
    percentile of its level over the file, and until: where it first sinks within 10 dB of that floor, by 4 s (NaN
    if that leaves under half a second)."""
    win = max(0.04, 4.0 / f1)
    n = int(win * rate)
    x = stereo[ev.onset(stereo.mean(axis=1)):]
    w = np.hanning(n)
    basis = np.exp(-2j * np.pi * np.outer(np.arange(n) / rate, freqs)) * w[:, None] * (2 / w.sum())
    def track(centres):
        starts = np.clip((np.asarray(centres) * rate).astype(int) - n // 2, 0, len(x) - n)
        idx = starts[:, None] + np.arange(n)[None, :]
        z = np.stack([x[idx, c] @ basis for c in (0, 1)])  # channel x frame x partial
        return 10 * np.log10(np.sum(np.abs(z) ** 2, axis=0) + 1e-24)
    early = np.arange(max(win / 2, 0.05), 0.4, 0.01)
    slope, start = np.polyfit(early, track(early), 1)
    late_t = np.arange(1.0, min(4.0, len(x) / rate - win), 0.05)
    late = track(late_t)
    if until is None:
        floor = np.percentile(track(np.arange(win / 2, len(x) / rate - win, 0.05)), 5, axis=0)
        held = late > floor + 10
        until = np.array([late_t[np.argmin(h)] if not h.all() else late_t[-1] for h in held.T])
        until[until < 1.5] = np.nan
    else:
        floor = None
    decay, knee = np.full(len(freqs), np.nan), np.full(len(freqs), np.nan)
    for i, t in enumerate(until):
        if np.isfinite(t) and (late_t <= t).sum() >= 10:
            decay[i], at = np.polyfit(late_t[late_t <= t] - 1.0, late[late_t <= t, i], 1)
            knee[i] = at - (start[i] + slope[i] * 0.1)
    return {"level": start + slope * 0.1, "early": slope, "late": decay, "knee": knee, "floor": floor, "until": until}


def reference():
    if CACHE.exists():
        return np.load(CACHE, allow_pickle=True)["data"].item()
    def one(item):
        midi, layer = item
        ks, fs = partials(midi)
        return item, measure(*ev.reference(midi, layer), fs, CAL[midi][0])
    with ThreadPoolExecutor(8) as pool:
        data = dict(pool.map(one, [(m, l) for m in KEYS for l in (1, 2, 3, 4)]))
    np.savez(CACHE, data=np.array(data, dtype=object))
    return data


def model(ref):
    """Every key and layer rendered for 5 s, the key held (pedal down, no sympathy: the strings alone), measured as
    its recording was, over the same late window."""
    def one(item):
        midi, layer = item
        ks, fs = partials(midi)
        return item, measure(*ev.render(midi, layer, 5.0, pedal=1, sympathy=0.0), fs, CAL[midi][0], ref[item]["until"])
    for old in ev.SCRATCH.glob("*.wav"):
        old.unlink()
    with ThreadPoolExecutor(8) as pool:
        return dict(pool.map(one, [(m, l) for m in KEYS for l in (1, 2, 3, 4)]))


# ---- the header -------------------------------------------------------------------------------------------------

def analytic_strike(midi):
    return float(np.clip(0.115 + (midi - 60) * 0.00045, 0.075, 0.145))


def read_header():
    if not HEADER.exists():
        return [analytic_strike(m) for m in KEYS], 0.0, *flat()
    text = HEADER.read_text()
    def array(name):
        body = text.split(f"{name}{{")[1].split(";")[0]
        return np.array([float(v) for v in re.findall(r"-?\d+(?:\.\d+)?(?:e-?\d+)?", body)])
    spread = float(re.search(r"strikeSpread = ([\d.]+);", text).group(1))
    return (list(array("strikePositions")), spread, array("radiationDb").reshape(88, BANDS),
            array("partialDb").reshape(88, PARTIALS), array("velocityDb").reshape(88, 2, BANDS),
            array("brighteningDb").reshape(88, 2, BANDS), array("promptDecayLog2").reshape(88, PARTIALS),
            array("aftersoundDecayLog2").reshape(88, PARTIALS), array("aftersoundLevelDb").reshape(88, PARTIALS))


def flat():
    return (np.zeros((88, BANDS)), np.zeros((88, PARTIALS)), np.zeros((88, 2, BANDS)), np.zeros((88, 2, BANDS)),
            np.zeros((88, PARTIALS)), np.zeros((88, PARTIALS)), np.zeros((88, PARTIALS)))


def split(data, field):
    return {k: v[field] for k, v in data.items()}


def write_header(strike, spread, radiation, partial, velocity, brightening, prompt, aftersound, knee, note):
    def rows(table, digits=1):
        return ",\n".join("    {{" + ", ".join(f"{v:.{digits}f}" for v in row) + "}}" for row in table)
    def layers(table):
        return ",\n".join("  {{" + ",\n".join("    {{" + ", ".join(f"{v:.1f}" for v in layer) + "}}" for layer in key) + "}}"
                           for key in table)
    text = f"""#pragma once

// Generated by tools/fit_piano.py ({note}). Do not edit.
#include <array>
#include <cstddef>

namespace pianocraft {{

// The tables' frequencies: band b is {LOWEST:g} Hz x 2^(b / {BANDS_PER_OCTAVE}).
inline constexpr double spectrumLowest = {LOWEST};
inline constexpr double spectrumBandsPerOctave = {BANDS_PER_OCTAVE};
inline constexpr std::size_t spectrumBands = {BANDS};

// Each key's strike position, as a fraction of its speaking length, A0 first, and how far strikes spread about
// it (one standard deviation, the same fraction): a unison's strings and the felt's contact don't meet at a point.
inline constexpr std::array<double, 88> strikePositions{{
    {", ".join(f"{v:.4f}" for v in strike)}}};
inline constexpr double strikeSpread = {spread:.4f};

// The radiation, dB: what the board and microphones make of each frequency of each key's bridge force.
inline constexpr std::array<std::array<float, spectrumBands>, 88> radiationDb{{{{
{rows(radiation)}}}}};

// Each key's own partials, dB on top of the radiation, for partials 1-{PARTIALS}: what the recording holds that a
// smooth law can't (its board's resonances and the comb's detail), measured at velocity 80.
inline constexpr std::size_t spectrumPartials = {PARTIALS};
inline constexpr std::array<std::array<float, spectrumPartials>, 88> partialDb{{{{
{rows(partial)}}}}};

// How each frequency of each key's strike changes with velocity, dB re velocity 80: at 40 and at 120 (of 127).
inline constexpr std::array<std::array<std::array<float, spectrumBands>, 2>, 88> velocityDb{{{{
{layers(velocity)}}}}};

// How much the recordings brighten with velocity, dB re the fundamental and re velocity 80, at 40 and at 120:
// Velocity Brightness scales this, 1 leaving the calibrated piano.
inline constexpr std::array<std::array<std::array<float, spectrumBands>, 2>, 88> brighteningDb{{{{
{layers(brightening)}}}}};

// Each key's partials' decay times, log2 of a factor on the engine's prompt and aftersound laws, for partials
// 1-{PARTIALS}: what the recording's early (0.05-0.4 s) and late (1-4 s) decays need.
inline constexpr std::array<std::array<float, spectrumPartials>, 88> promptDecayLog2{{{{
{rows(prompt, 2)}}}}};
inline constexpr std::array<std::array<float, spectrumPartials>, 88> aftersoundDecayLog2{{{{
{rows(aftersound, 2)}}}}};
// And the aftersound's level, dB on the engine's (its unison's slow mode at -18 dB, -35 in the top octave).
inline constexpr std::array<std::array<float, spectrumPartials>, 88> aftersoundLevelDb{{{{
{rows(knee)}}}}};

}} // namespace pianocraft
"""
    HEADER.write_text(text)
    subprocess.run(["cmake", "--build", str(ROOT / "build"), "--target", "pianocraft-render"], check=True,
                   capture_output=True)


# ---- fits -------------------------------------------------------------------------------------------------------

def comb(ks, x, spread):
    """The strike's comb in dB: sin^2(k pi x) averaged over strike points spread +-spread (Gaussian, as a fraction
    of the string), as a unison's strings and the felt's contact wander. Its notches fill in up the series."""
    return 10 * np.log10(np.maximum((1 - np.cos(2 * np.pi * ks * x) * np.exp(-2 * (np.pi * ks * spread) ** 2)) / 2, 1e-4))


def fit_strike():
    """Per key: the strike position, between 1/16 and 1/7 of the string, and its spread, whose comb best explains
    the recordings' partials: from the 1st, while layers 1, 3 and 4 all hold them, up to the 30th, each layer
    detrended by a running median over 5 partials (which leaves the comb and drops the envelope), then averaged.
    A key counts with 15 partials and a comb that explains 30% of what is left; the others take their nearest
    fitted neighbours' position, and every key the fitted keys' median spread."""
    from scipy.ndimage import median_filter
    data = reference()
    lv, fl = split(data, "level"), split(data, "floor")
    grid = np.arange(0.0625, 0.1429, 0.0005)
    def detrend(y):
        return y - median_filter(y, size=5, mode="nearest")
    found = {}
    for midi in KEYS:
        ks, fs = partials(midi)
        held = np.all([lv[midi, layer] > fl[midi, layer] + 10 for layer in (1, 3, 4)], axis=0) & (ks <= 30)
        n = int(np.argmin(held)) if not held.all() else len(held)
        if n < 15:
            continue
        k = ks[:n].astype(float)
        r = np.mean([detrend(lv[midi, layer][:n]) for layer in (1, 3, 4)], axis=0)
        errors = np.array([[np.sum((r - detrend(comb(k, x, w))) ** 2) for w in SPREADS] for x in grid])
        i, j = np.unravel_index(np.argmin(errors), errors.shape)
        explained = 1 - errors[i, j] / np.sum(r ** 2)
        if explained >= 0.3:
            found[midi] = (grid[i], explained, SPREADS[j])
    keys = np.array(sorted(found))
    xs = np.array([found[m][0] for m in keys])
    strike = [found[m][0] if m in found else float(np.interp(m, keys, xs)) for m in KEYS]
    spread = float(np.median([found[m][2] for m in keys]))
    for m in KEYS:
        mark = f"fit, explains {found[m][1]:.0%}, spread {found[m][2]:.4f}" if m in found else "neighbours"
        print(f"{ev.note_name(m):4s} strike {strike[m - 21]:.4f} (1/{1 / strike[m - 21]:.1f}), analytic {analytic_strike(m):.4f}: {mark}")
    print(f"{len(found)} keys fitted; spread {spread:.4f}")
    return strike, spread


def bilinear(keys, freqs, key_knots, band_knots):
    """Sparse weights of each (key, frequency) point on a grid of knots, bilinear."""
    band = np.log2(np.asarray(freqs) / LOWEST) * BANDS_PER_OCTAVE
    def axis(v, knots):
        i = np.clip(np.searchsorted(knots, v) - 1, 0, len(knots) - 2)
        t = np.clip((v - knots[i]) / (knots[i + 1] - knots[i]), 0, 1)
        return i, t
    ki, kt = axis(np.asarray(keys, float), key_knots)
    bi, bt = axis(band, band_knots)
    nb = len(band_knots)
    rows, cols, vals = [], [], []
    for dk, wk in ((0, 1 - kt), (1, kt)):
        for db, wb in ((0, 1 - bt), (1, bt)):
            rows.append(np.arange(len(band)))
            cols.append((ki + dk) * nb + bi + db)
            vals.append(wk * wb)
    return sparse.csr_matrix((np.concatenate(vals), (np.concatenate(rows), np.concatenate(cols))),
                             shape=(len(band), len(key_knots) * nb))


def penalty(nk, nb, along_key, along_band):
    """Second differences along both axes of an nk x nb grid."""
    def second(n):
        return sparse.diags([1, -2, 1], [0, 1, 2], shape=(n - 2, n))
    return sparse.vstack([along_key * sparse.kron(second(nk), sparse.eye(nb)),
                          along_band * sparse.kron(sparse.eye(nk), second(nb))])


def evaluate_grid(coef, key_knots, band_knots):
    keys = np.repeat(KEYS, BANDS)
    freqs = np.tile(LOWEST * 2 ** (np.arange(BANDS) / BANDS_PER_OCTAVE), 88)
    return (bilinear(keys, freqs, key_knots, band_knots) @ coef).reshape(88, BANDS)


def smooth_fit(m, f, d, w, keys, bands, smooth):
    """A smooth surface over (key, frequency), bilinear on knots, weighted least squares with second-difference
    smoothing, evaluated at every key's every band."""
    a = sparse.diags(np.sqrt(w)) @ bilinear(m, f, keys, bands)
    p = penalty(len(keys), len(bands), smooth, smooth)
    coef = lsqr(sparse.vstack([a, p]), np.concatenate([np.sqrt(w) * d, np.zeros(p.shape[0])]))[0]
    return coef, evaluate_grid(coef, keys, bands)


LN1000 = np.log(1000.0)


def prompt_law(f):  # InstrumentModel: promptDecaySeconds, aftersoundDecaySeconds and the slow mode's level
    return 4.89 * (f / 440.0) ** -0.745


def aftersound_law(f):
    return 16.87 * (f / 440.0) ** -0.444


def partner_db(midi):
    return -18.0 if midi < 85 else -18.0 - 17.0 * min(1.0, (midi - 85) / 15.0)


def fit_decays():
    """Each partial's envelope at layer 3, fitted directly: its power, averaged over beats, as
    A e^-2at + B e^-2bt over its own floor, from 0.03 s (or half the analysis window) to 5 s, in dB with a soft
    loss. A calibrated partial is a coupled unison: a its prompt decay, b its aftersound, B/A its slow mode's level.
    An extended partial is one string mode: one exponential. A fit counts where the partial rises 10 dB over its
    floor (at 20, the bass's upper partials, which ring on 15-20 dB over it for seconds, all took the surface and
    died 30 dB early by 1.6 s) and the fit stays within 4 dB rms; the others take a smooth surface through the counted ones (keys outside
    the split, knots every 6 keys and 1/2 octave). Returns the three tables, as in the header."""
    from scipy.optimize import least_squares
    ref = reference()

    def one(midi):
        ks, fs = partials(midi)
        f1 = CAL[midi][0]
        rate, stereo = ev.reference(midi, 3)
        win = max(0.04, 4.0 / f1)
        n = int(win * rate)
        x = stereo[ev.onset(stereo.mean(axis=1)):]
        w = np.hanning(n)
        basis = np.exp(-2j * np.pi * np.outer(np.arange(n) / rate, fs)) * w[:, None] * (2 / w.sum())
        t = np.arange(max(win / 2, 0.03), min(5.0, len(x) / rate - win), 0.02)
        idx = (t * rate).astype(int)[:, None] - n // 2 + np.arange(n)[None, :]
        z = np.stack([x[idx, c] @ basis for c in (0, 1)])
        db = 10 * np.log10(np.sum(np.abs(z) ** 2, axis=0) + 1e-24)
        floor = ref[midi, 3]["floor"]
        rows = []
        for i, (k, f) in enumerate(zip(ks, fs)):
            y, noise = db[:, i], 10 ** (floor[i] / 10)
            if y.max() < floor[i] + 10:
                rows.append(None)
                continue
            pair = int(k) in CAL[midi][2] and len([c for c in CAL[midi][2] if c <= k]) <= 32
            # Bounds: the prompt decay time within 1/8 and 16 times its law (the bass's upper partials ring far past 4
            # times), the aftersound's at most 3 times its own: within 5 s a slower, quieter aftersound is
            # indistinguishable from a plateau of noise or other strings.
            fast, slow = LN1000 / (prompt_law(f) / 8), LN1000 / (16 * prompt_law(f))
            a0 = np.clip(-np.polyfit(t[t < 0.4], y[t < 0.4], 1)[0] / 8.686, slow, fast)
            level0 = y[0] + 8.686 * a0 * t[0]
            least_after = LN1000 / (3 * aftersound_law(f))
            if pair:
                def curve(p):
                    a, b, l0, rdb = p
                    return 10 * np.log10(10 ** (l0 / 10) * np.exp(-2 * a * t) +
                                         10 ** ((l0 + rdb) / 10) * np.exp(-2 * min(a, b) * t) + noise)
                start = [a0, max(least_after, 0.2 * a0), level0, -20.0]
                bounds = ([slow, least_after, level0 - 40, -60.0], [fast, fast, level0 + 40, -3.0])
            else:
                def curve(p):
                    a, l0 = p
                    return 10 * np.log10(10 ** (l0 / 10) * np.exp(-2 * a * t) + noise)
                start, bounds = [a0, level0], ([slow, level0 - 40], [fast, level0 + 40])
            fit = least_squares(lambda p: curve(p) - y, np.clip(start, *bounds), bounds=bounds, loss="soft_l1", f_scale=3.0)
            rms = np.sqrt(np.mean((curve(fit.x) - y)[y > floor[i] + 6] ** 2))
            if rms > 4.0:
                rows.append(None)
                continue
            a = fit.x[0]
            if pair:
                rows.append((np.log2(LN1000 / a / prompt_law(f)), np.log2(LN1000 / min(a, fit.x[1]) / aftersound_law(f)),
                             fit.x[3] - partner_db(midi)))
            else:
                rows.append((np.log2(LN1000 / a / prompt_law(f)), np.nan, np.nan))
        return midi, rows

    with ThreadPoolExecutor(8) as pool:
        fitted = dict(pool.map(one, KEYS))
    tables = [np.zeros((88, PARTIALS)) for _ in range(3)]
    key_knots, bands = np.arange(21, 115, 6.0), np.arange(0, BANDS + 6, 6.0)
    for column, table in enumerate(tables):
        rows = [(m, f, r[column], 1.0) for m in ev.FIT_KEYS for f, r in zip(partials(m)[1], fitted[m])
                if r is not None and np.isfinite(r[column])]
        coef, _ = smooth_fit(*(np.array(c) for c in zip(*rows)), key_knots, bands, 2.0)
        for m in KEYS:
            ks, fs = partials(m)
            surface = bilinear(np.full(len(fs), m), fs, key_knots, bands) @ coef
            for k, v, r in zip(ks, surface, fitted[m]):
                if k <= PARTIALS:
                    table[m - 21, k - 1] = r[column] if r is not None and np.isfinite(r[column]) else v
        print(f"{['prompt', 'aftersound', 'aftersound level'][column]}: {len(rows)} partials fitted on fitting keys, "
              f"median {np.median([r[2] for r in rows]):+.2f}{'' if column == 2 else ' (log2)'}")
    return np.clip(tables[0], -3, 3), np.clip(tables[1], -3, 3), np.clip(tables[2], -20, 15)


def fit_spectrum(rounds):
    """From flat tables (keeping the strike positions), each round renders every key and layer and refines:
    - the radiation: a smooth surface (knots every 6 keys, 1/6 octave) through layer 3's level differences,
      recording minus model, less their median, so the instrument's loudness stays the engine's. Each partial weighs
      by the square root of its amplitude re the note's strongest (at least 0.05);
    - each key's partials: what is left of its own differences, in full where the recording holds the partial 20 dB
      over its floor, fading to none at 10 dB, within +-25 dB;
    - the velocity law: smooth surfaces (knots every 12 keys, uniform weights) through each partial's difference at
      layers 1 and 4 less its difference at layer 3;
    A partial the recording doesn't hold over its floor mustn't sound over that floor: it pulls down to it. The
    smooth fits use the keys outside the split; every key takes its own partials (measured, as their frequencies
    are). The brightening table is the recordings' own: the velocity surfaces through each partial's level change
    from layer 3, re its key's fundamental."""
    strike, spread, *_, prompt, aftersound, knee = read_header()
    radiation, partial, velocity, brightening, *_ = flat()
    ref = reference()
    lv, fl = split(ref, "level"), split(ref, "floor")
    key_knots, band_knots, vk = np.arange(21, 115, 6.0), np.arange(0, BANDS + 2, 2.0), np.arange(21, 121, 12.0)

    def held(midi, layer, margin=10):
        return lv[midi, layer] > fl[midi, layer] + margin

    for i, layer in enumerate(FIT_LAYERS):
        rows = []
        for midi in ev.FIT_KEYS:
            ks, fs = partials(midi)
            ok = held(midi, layer) & held(midi, 3)
            change = lv[midi, layer] - lv[midi, 3]
            if ok[0]:
                rows += [(midi, f, c - change[0], 1.0) for f, c in zip(fs[ok], change[ok])]
        brightening[:, i] = smooth_fit(*(np.array(c) for c in zip(*rows)), vk, band_knots, 3.0)[1]

    for round_ in range(rounds):
        write_header(strike, spread, radiation, partial, velocity, brightening, prompt, aftersound, knee,
                     f"spectrum, round {round_}" if round_ else "spectrum, flat")
        result = model(ref)
        mv = split(result, "level")
        diff = {(m, l): lv[m, l] - mv[m, l] for m in KEYS for l in (1, 3, 4)}
        for (m_, l_), d_ in diff.items():
            over = ~held(m_, l_) & (mv[m_, l_] > fl[m_, l_])
            d_[over] = np.minimum(fl[m_, l_][over] - mv[m_, l_][over], 0)
        # Levels at layer 3: the radiation and each key's partials.
        rows = []
        for midi in ev.FIT_KEYS:
            ks, fs = partials(midi)
            ok = (held(midi, 3) | (mv[midi, 3] > fl[midi, 3])) & (mv[midi, 3] > mv[midi, 3].max() - 100)
            weight = np.maximum(10 ** ((lv[midi, 3] - lv[midi, 3][held(midi, 3)].max()) / 40), 0.05)
            rows += [(midi, f, d, w) for f, d, w in zip(fs[ok], diff[midi, 3][ok], weight[ok])]
        m, f, d, w = (np.array(c) for c in zip(*rows))
        offset = np.median(d)
        coef, grid = smooth_fit(m, f, d - offset, w, key_knots, band_knots, 2.0)
        radiation += grid
        for midi in KEYS:
            ks, fs = partials(midi)
            surface = bilinear(np.full(len(fs), midi), fs, key_knots, band_knots) @ coef
            trust = np.clip((lv[midi, 3] - fl[midi, 3] - 10) / 10, 0, 1)
            left = np.clip(diff[midi, 3] - offset - surface, -25, 25) * trust
            for k, v in zip(ks, left):
                if k <= PARTIALS:
                    partial[midi - 21, k - 1] = np.clip(partial[midi - 21, k - 1] + v, -25, 25)
        # The velocity law, layers 1 and 4.
        for i, layer in enumerate(FIT_LAYERS):
            rows = []
            for midi in ev.FIT_KEYS:
                ks, fs = partials(midi)
                ok = (held(midi, layer) | (mv[midi, layer] > fl[midi, layer])) & held(midi, 3)
                rows += [(midi, f, d, 1.0) for f, d in zip(fs[ok], (diff[midi, layer] - diff[midi, 3])[ok])]
            velocity[:, i] += smooth_fit(*(np.array(c) for c in zip(*rows)), vk, band_knots, 3.0)[1]
        radiation, velocity = np.clip(radiation, -40, 40), np.clip(velocity, -60, 30)
        print(f"round {round_ + 1}: " + score(result, quiet=True))
    write_header(strike, spread, radiation, partial, velocity, brightening, prompt, aftersound, knee,
                 f"spectrum, {rounds} rounds")


def score(result=None, quiet=False):
    """Each partial's level error (level-matched on each note's three strongest) and its decays' error (log2 of the
    slopes' ratio), fitting and split keys, by layer; with result, of that model run."""
    ref = reference()
    result = result or model(ref)
    out = []
    for name, keys in (("fit", ev.FIT_KEYS), ("split", sorted(ev.HELD_OUT_KEYS))):
        for layer in (1, 2, 3, 4):
            errors, early, late, knees = [], [], [], []
            for midi in keys:
                r, mm = ref[midi, layer], result[midi, layer]
                ok = r["level"] > r["floor"] + 10
                e = mm["level"][ok] - r["level"][ok]
                if ok.sum() >= 2:
                    order = np.argsort(r["level"][ok])[::-1][:3]
                    errors += list(np.abs(e - np.median(e[order])))
                strong = r["level"] > r["floor"] + 20
                for field, store in (("early", early), ("late", late)):
                    g = strong & (r[field] < -0.3) & (mm[field] < -0.3)
                    store += list(np.abs(np.log2(mm[field][g] / r[field][g])))
                g = strong & np.isfinite(r["knee"]) & np.isfinite(mm["knee"])
                knees += list(np.abs(mm["knee"][g] - r["knee"][g]))
            out.append(f"{name} L{layer} {np.mean(errors):.1f} dB, decay x{2 ** np.mean(early):.2f}/x{2 ** np.mean(late):.2f}, "
                       f"knee {np.mean(knees):.1f} dB")
    line = "partial level | early/late decay | knee error: " + "; ".join(out)
    if not quiet:
        print(line)
    return line


if __name__ == "__main__":
    command, *args = sys.argv[1:] or ["help"]
    if command == "neutral":
        write_header([analytic_strike(m) for m in KEYS], 0.0, *flat(), "neutral")
    elif command == "strike":
        write_header(*fit_strike(), *read_header()[2:], "strike positions")
    elif command == "decays":
        strike, spread, radiation, partial, velocity, brightening, *_ = read_header()
        write_header(strike, spread, radiation, partial, velocity, brightening, *fit_decays(), "decays")
    elif command == "spectrum":
        fit_spectrum(int(args[0]) if args else 3)
    elif command == "score":
        score()
    else:
        sys.exit(__doc__)
