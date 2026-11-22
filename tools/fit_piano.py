#!/usr/bin/env python3
"""Fit the upright's spectrum to the Meta Piano corpus: each key's strike position, the radiation (board and
microphones) and how the hammer brightens with velocity. Writes src/model/SpectrumCalibration.hpp, and with tune,
src/model/PartialTuning.hpp.

    .venv/bin/python tools/fit_piano.py measure       a new piano's calibration: fundamentals, stiffness, partials
    .venv/bin/python tools/fit_piano.py tune          each partial's frequency, followed up the recordings' series
    .venv/bin/python tools/fit_piano.py neutral       the analytic strike law and flat tables
    .venv/bin/python tools/fit_piano.py strike        fit each key's strike position from its recordings' notches
    .venv/bin/python tools/fit_piano.py decays        fit each partial's two-stage decay to its recorded envelope
    .venv/bin/python tools/fit_piano.py spectrum [n] [resume]
                                                      n rounds (default 3): render, compare, refine the level tables,
                                                      from flat tables or, with resume, the header's
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
from scipy.optimize import least_squares
from scipy.sparse.linalg import lsqr

sys.path.insert(0, str(Path(__file__).resolve().parent))
import evaluate_model as ev  # noqa: E402
from pianos import PIANOS  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
PIANO = ev.PIANO  # PIANOCRAFT_PIANO (tools/pianos.py)
HEADER = PIANO.spectrum_header
TUNING = PIANO.tuning_header
CACHE = PIANO.cache
KEYS = list(range(21, 109))
FIT_LAYERS = (1, 4)  # layer 3 is the reference (0 dB); layer 2 is held out
TOP = 9000.0         # highest partial compared and synthesized
LOWEST, BANDS_PER_OCTAVE, BANDS = 25.0, 12, 110  # the tables' frequencies: 25 Hz up, 1/12 octave
PARTIALS = 112
SPREADS = np.array([0.0005, 0.001, 0.002, 0.003, 0.004, 0.005, 0.006, 0.008, 0.01])


def calibration_of(piano):
    if not piano.calibration_header.exists():
        return {}
    blocks = piano.calibration_header.read_text().split("CalibrationData{")[1:]
    out = {}
    for midi, block in zip(KEYS, blocks):
        f1, b = (float(v) for v in block.split(",")[:2])
        partials = {int(k): float(f) for k, f in re.findall(r"PartialCalibration\{(\d+), ([\d.e-]+)", block) if int(k) > 0}
        out[midi] = (f1, b, partials)
    return out


CAL = calibration_of(PIANO)


def read_tuning():
    if not TUNING.exists():
        return np.zeros((88, PARTIALS))
    body = TUNING.read_text().split("partialCents{")[1].split(";")[0]
    values = [float(v) for v in re.findall(r"-?\d+(?:\.\d+)?", body)]
    return np.array(values).reshape(88, PARTIALS) if values else np.zeros((88, PARTIALS))


TUNE = read_tuning()


def partials(midi, tuned=True):
    """The model's partial series: the calibrated partials, then the inharmonic law, each moved by its measured
    tuning (a partial past the table keeps the last one's), to TOP (InstrumentModel)."""
    f1, b, calibrated = CAL[midi]
    shift = (lambda k: 2 ** (TUNE[midi - 21, min(k, PARTIALS) - 1] / 1200)) if tuned else (lambda k: 1.0)
    series = {k: f * shift(k) for k, f in calibrated.items() if 0 < f * shift(k) <= TOP}
    k = max(calibrated) + 1
    while len(series) < 112:
        f = k * f1 * np.sqrt((1 + b * k * k) / (1 + b)) * shift(k)
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
        data = dict(pool.map(one, [(m, l) for m in KEYS for l in sorted(PIANO.layers) if PIANO.has(m, l)]))
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
        return dict(pool.map(one, list(ref)))


# ---- a new piano's calibration ----------------------------------------------------------------------------------

def measure_key(midi, prior_b):
    """A key from its recording (mf, else ff, else pp), over 0.05-1.55 s. From C4 up the fundamental dominates: it is
    the strongest peak within 60 cents of the key, and the partials above are searched +-0.04 f1 about the stiff-string
    law from it (at prior_b). Below, a wound string's fundamental is weak (A#0's took a peak 180 cents off): the series
    is followed up from where the law puts it, searched +-0.15 f1, refitted f_k = k F sqrt(1 + B k^2) as partials are
    found. Either way a partial counts where it is a peak (not its span's edge) 12 dB over that span's median, to the
    32nd and 16 kHz. Also the fundamental's decay, a line through its power over 0.2-3 s, and the channels' balance over
    0.05-1.05 s."""
    layer = next((l for l in (3, 4, 1) if PIANO.has(midi, l)), None)
    if layer is None:
        return None
    rate, x = ev.reference(midi, layer)
    x = x[ev.onset(x.mean(axis=1)):]
    mono = x.mean(axis=1)
    seg = mono[int(0.05 * rate):int(1.55 * rate)]
    n = 1 << 19
    s = 10 * np.log10(np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n)) ** 2 + 1e-30)
    f = np.fft.rfftfreq(n, 1 / rate)
    nominal = 440.0 * 2 ** ((midi - 69) / 12)

    def peak(guess, width):
        span = np.flatnonzero(np.abs(f - guess) < width)
        j = int(np.argmax(s[span]))
        i = span[j]
        if s[i] > np.median(s[span]) + 12 and 0 < j < len(span) - 1:
            left, mid, right = s[i - 1], s[i], s[i + 1]
            return f[i] + 0.5 * (left - right) / (left - 2 * mid + right) * (f[1] - f[0])
        return None

    found, b = {}, prior_b
    if midi >= 60:
        span = np.flatnonzero(np.abs(1200 * np.log2(np.maximum(f, 1.0) / nominal)) < 60)
        f1 = f[span[np.argmax(s[span])]]
        found[1] = peak(f1, 0.04 * nominal) or f1
        big_f = found[1] / np.sqrt(1 + b)
        for k in range(2, 33):
            guess = k * big_f * np.sqrt(1 + b * k * k)
            if guess > min(16000.0, 0.45 * rate):
                break
            hit = peak(guess, 0.04 * found[1])
            if hit is not None:
                found[k] = hit
    else:
        big_f, misses = nominal / np.sqrt(1 + b), 0
        for k in range(1, 33):
            guess = k * big_f * np.sqrt(1 + b * k * k)
            if guess > min(16000.0, 0.45 * rate):
                break
            hit = peak(guess, 0.15 * nominal)
            if hit is None:
                misses += 1
                if misses >= 4 and k > 8:
                    break
                continue
            found[k], misses = hit, 0
            if len(found) >= 3:
                ks = np.array(sorted(found), dtype=float)
                fs = np.array([found[int(j)] for j in ks])
                fit = least_squares(lambda p: ks * p[0] * np.sqrt(1 + p[1] * ks * ks) - fs, [big_f, b],
                                    bounds=([0.8 * big_f, 1e-6], [1.2 * big_f, 0.02]), loss="soft_l1", f_scale=1.0)
                big_f, b = fit.x
    if not found:
        return None
    if len(found) >= 3:
        ks = np.array(sorted(found), dtype=float)
        fs = np.array([found[int(j)] for j in ks])
        b = least_squares(lambda p: ks * p[0] * np.sqrt(1 + p[1] * ks * ks) - fs, [big_f, b],
                          bounds=([0.8 * big_f, 1e-6], [1.2 * big_f, 0.02]), loss="soft_l1", f_scale=1.0).x[1]
    f1 = found.get(1, big_f * np.sqrt(1 + b))
    w = int(max(0.04, 4 / f1) * rate)
    times = np.arange(0.2, min(3.0, len(mono) / rate - 0.1), 0.05)
    basis = np.exp(-2j * np.pi * f1 * np.arange(w) / rate) * np.hanning(w)
    level = np.array([10 * np.log10(sum(np.abs(x[int(t * rate):int(t * rate) + w, c] @ basis) ** 2 for c in (0, 1)) + 1e-30)
                      for t in times])
    slope = np.polyfit(times, level, 1)[0] if len(times) >= 6 else -10.0
    body = x[int(0.05 * rate):int(1.05 * rate)]
    return {"found": found, "b": b, "decay": float(np.clip(-60.0 / min(slope, -1.0), 0.3, 40.0)),
            "balance": 10 * np.log10(np.mean(body[:, 0] ** 2) / np.mean(body[:, 1] ** 2))}


def fit_calibration():
    """Each key's calibration for a piano other than Meta's (whose came from tools/calibrate.py), from measure_key():
    - stiffness B: where 6 partials or more were found and the fit stayed inside its bounds, the median of those keys
      within +-4; above the last of them (the top octaves' few partials leave B loose: fits there ran from 1e-6 to 0.02
      between neighbours), the reliable keys' log-linear rise from C4, continued, at most 0.02;
    - from C4 up the fundamental as found; below, the series' F refitted at that B as the median of
      f_k / (k sqrt(1 + B k^2)) over its partials 2-16, and the fundamental F sqrt(1 + B); partials 1-8 more than 25
      cents off the law dropped, the ones above kept as found (the bass's wound strings drift flat of it, as fit_tuning
      follows); the partials to the last found, the others on the law;
    - stereo gains that bring the model's pan (InstrumentModel's spaced pair) to the recording's balance, evened over 5
      keys as the tables are (microphones 8 inches over the strings swing it +-10 dB key to key), split between the
      channels, within 0.5-1.5;
    - the Meta piano's register velocity laws (1.27 / 1.627 / 2.195; the fitted velocity tables do the rest).
    A key without a recording takes its nearest measured neighbour's, moved by equal temperament."""
    prior = {m: b for m, (_, b, _) in calibration_of(PIANOS["meta"]).items()}
    with ThreadPoolExecutor(8) as pool:
        measured = dict(zip(KEYS, pool.map(lambda m: measure_key(m, prior[m]), KEYS)))
    reliable = {m: k["b"] for m, k in measured.items() if k and len(k["found"]) >= 6 and 2e-6 < k["b"] < 0.0195}
    rise = np.polyfit([m for m in reliable if m >= 60], np.log([b for m, b in reliable.items() if m >= 60]), 1)
    top = max(reliable)
    stiffness = {m: float(min(np.exp(np.polyval(rise, m)), 0.02)) if m > top
                 else float(np.median([b for n, b in reliable.items() if abs(n - m) <= 4] or [prior[m]])) for m in KEYS}
    balance = np.array([measured[min((n for n in KEYS if measured[n]), key=lambda n: abs(n - m))]["balance"] for m in KEYS])
    balance = even(balance[:, None])[:, 0]
    keys = []
    for midi in KEYS:
        near = min((m for m in KEYS if measured[m] is not None), key=lambda m: abs(m - midi))
        found, b = measured[near]["found"], stiffness[midi]
        law = lambda k, big_f: k * big_f * np.sqrt(1 + b * k * k)
        if near >= 60:
            big_f = found[1] / law(1, 1.0)
        else:
            big_f = float(np.median([f / law(k, 1.0) for k, f in found.items() if 2 <= k <= 16] or
                                    [f / law(k, 1.0) for k, f in found.items()]))
        kept = {k: f for k, f in found.items() if k > 8 or abs(1200 * np.log2(f / law(k, big_f))) <= 25}
        kept.setdefault(1, law(1, big_f))
        ratio = 2 ** ((midi - near) / 12)
        partial_list = [(k, kept[k] * ratio if k in kept else law(k, big_f) * ratio, k in kept and near == midi)
                        for k in range(1, max(kept) + 1) if law(k, big_f) * ratio < 16000.0]
        pan = np.clip((midi - 64.5) / 55.0, -0.55, 0.55)
        correction = balance[midi - 21] - 10 * np.log10((1 - pan) / (1 + pan))
        key = {"fundamental": partial_list[0][1], "b": b, "partials": partial_list, "decay": measured[near]["decay"],
               "left": float(np.clip(10 ** (correction / 40), 0.5, 1.5)), "right": float(np.clip(10 ** (-correction / 40), 0.5, 1.5)),
               "velocity": 1.27 if midi < 48 else (1.627 if midi < 72 else 2.195), "measured": near == midi}
        keys.append((midi, key))
        print(f"{ev.note_name(midi):4s} {'measured' if near == midi else 'from ' + ev.note_name(near):9s} f0 {key['fundamental']:8.3f} Hz "
              f"({1200 * np.log2(key['fundamental'] / (440 * 2 ** ((midi - 69) / 12))):+5.1f} c) B {b:.6f} "
              f"(fit {measured[near]['b']:.6f}) {sum(m for _, _, m in partial_list)}/{len(partial_list)} partials, "
              f"decay {key['decay']:5.1f} s, balance {balance[midi - 21]:+5.1f} dB")
    return keys


def write_calibration(keys):
    lines = ["#pragma once", "", '#include "model/PianoData.hpp"', "",
             f"// Generated by tools/fit_piano.py (measure) from {PIANO.title}'s recordings. Do not edit.",
             f"namespace pianocraft::pianos::{PIANO.name} {{", "",
             "inline constexpr BodyCalibration soundboardCalibration{", "    {{1, 1, 1}},", "    {{1, 1, 1}},", "    {{0, 0, 0}},", "};", "",
             "inline constexpr std::array<CalibrationData, 88> measuredCalibration{{"]
    for midi, key in keys:
        scalars = [key["fundamental"], key["b"], key["decay"], 1.0, 60000, key["velocity"], 0, 1, key["left"], key["right"]]
        lines.append("    CalibrationData{" + ", ".join(format(v, ".12g") for v in scalars) + f", {len(key['partials'])}, {{{{")
        for k, f, measured in key["partials"]:
            lines.append(f"        PartialCalibration{{{k}, {f:.12g}, {key['decay']:.12g}, 0, 0, 0, {'true' if measured else 'false'}, false, false}},")
        lines.append(f"    }}}}}}, // MIDI {midi} {ev.note_name(midi)}{'' if key['measured'] else ' (from a neighbour)'}")
    lines += ["}};", "", f"}} // namespace pianocraft::pianos::{PIANO.name}", ""]
    PIANO.folder.mkdir(parents=True, exist_ok=True)
    PIANO.calibration_header.write_text("\n".join(lines))


# ---- the header -------------------------------------------------------------------------------------------------

def analytic_strike(midi):
    return float(np.clip(0.115 + (midi - 60) * 0.00045, 0.075, 0.145))


def spectrum(midi, window):
    """Layers 3 and 4's power spectra over window (s after the onset) summed, dB; None if the files are shorter."""
    total = None
    for layer in (3, 4):
        rate, x = ev.reference(midi, layer)
        mono = x.astype(float).mean(axis=1)
        seg = mono[ev.onset(mono):][int(window[0] * rate):int(window[1] * rate)]
        if len(seg) < rate // 2:
            return None, None
        n = 1 << 19
        power = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n)) ** 2
        total = power if total is None else total + power
    return np.fft.rfftfreq(n, 1 / rate), 10 * np.log10(total + 1e-30)


def track(midi, window):
    """Each partial's frequency in the recording, layers 3 and 4's power spectra over window (s) summed: followed up
    the series from the fundamental, partial k searched +-0.2 f1 about where the last two found ones extrapolate it
    (along the model's own spacing), and taken at the strongest peak there if it stands 12 dB over that span's
    median and within 0.15 f1 of the guess."""
    f, s = spectrum(midi, window)
    if f is None:
        return {}
    ks, base = partials(midi, tuned=False)
    model, f1, found = dict(zip(ks, base)), CAL[midi][0], {}
    for k in ks:
        guess = model[k]
        if len(found) >= 2:
            a, b = sorted(found)[-2:]
            guess = found[b] + (model[k] - model[b]) * (found[b] - found[a]) / (model[b] - model[a])
        span = np.flatnonzero(np.abs(f - guess) < 0.2 * f1)
        i = span[np.argmax(s[span])]
        if s[i] > np.median(s[span]) + 12 and abs(f[i] - guess) < 0.15 * f1:
            left, mid, right = s[i - 1], s[i], s[i + 1]
            found[k] = f[i] + 0.5 * (left - right) / (left - 2 * mid + right) * (f[1] - f[0])
    return found


def fit_tuning():
    """Each key's partials, cents on the series the model would play (partials(tuned=False)), from the late
    spectrum (1-3 s), where phantoms and longitudinal modes have died and the transverse partials ring on; gaps
    between them from the early one (0.05-1.55 s), and short notes (under 8 late partials) from it alone. A partial
    more than 12 cents a partial off the last one kept (or a first one 30 cents off) was lost, and is dropped. Between
    kept partials linear in k. A key moves only if 15 of its partials stand 6 dB stronger in the recording at their
    tracked frequencies than at the model's (a track hopping between a unison's strings, from C2 up, finds both on a
    peak); the others keep the model's series, which the calibration measured. Above the last kept partial, if past
    the 25th, the law (f/k)^2 = a + b k^2 fitted to the upper half of those off the model's peaks, continuous with it
    and never sharper; otherwise its offset holds. A key 45 cents off its moved neighbours at the 30th or 40th
    partial keeps the model's series too. The bass's wound strings run flat of the stiff-string law, which the calibration
    held above its 16th-or-so measured partial: D1 -20 cents at its 20th partial, -77 at the 30th, -214 at the
    50th (a stiff beam's shear and rotary inertia, which the law leaves out)."""
    table = np.zeros((88, PARTIALS))
    for midi in [m for m in KEYS if PIANO.has(m, 3) and PIANO.has(m, 4)]:
        late, early = track(midi, (1.0, 3.0)), track(midi, (0.05, 1.55))
        found = late if len(late) >= 8 else early
        if len(found) < 2:
            continue
        last = max(found)
        found = {**{k: v for k, v in early.items() if k < last}, **found}
        f1, b, calibrated = CAL[midi]
        base = lambda k: calibrated.get(k, 0) or k * f1 * np.sqrt((1 + b * k * k) / (1 + b))
        kept = {}
        for k in sorted(found):  # a lost partial jumps; the real series drifts ~6 cents a partial at most
            c = 1200 * np.log2(found[k] / base(k))
            previous = max(kept) if kept else None
            if (abs(c) <= 30 if previous is None else abs(c - kept[previous]) <= 12 * (k - previous)):
                kept[k] = c
        # A partial moves only where the recording holds it there and not where the model has it, 6 dB more: a
        # track hopping to a unison's other string finds both on a peak, and leaves it.
        f, s_ = spectrum(midi, (1.0, 3.0) if len(late) >= 8 else (0.05, 1.55))
        level = lambda hz: s_[np.abs(f - hz) <= 0.5].max()
        moved = {k for k, c in kept.items() if level(found[k]) >= level(base(k)) + 6}
        if len(moved) < 15:
            print(f"{ev.note_name(midi):4s} {len(moved):3d} partials off the model's peaks: the model's series")
            continue
        kept.setdefault(1, 0.0)  # the fundamental is calibrated
        found = {k: base(k) * 2 ** (c / 1200) for k, c in kept.items()}
        known, last = np.array(sorted(kept)), max(kept)
        cents = np.array([kept[k] for k in known])
        upper = np.array(sorted(k for k in moved if k >= last / 2))
        law = None
        if last >= 25 and len(upper) >= 4:
            y = (np.array([found[k] for k in upper]) / upper) ** 2
            slope, intercept = np.polyfit(upper.astype(float) ** 2, y, 1)
            law = lambda k: k * np.sqrt(intercept + max(slope, 0.0) * k * k)
        for k in range(1, PARTIALS + 1):
            if k <= last:
                table[midi - 21, k - 1] = np.interp(k, known, cents)
            elif law is not None:  # never sharper than the last: the wound strings only drift flat
                table[midi - 21, k - 1] = min(1200 * np.log2(law(k) / law(last) * found[last] / base(k)), cents[-1])
            else:
                table[midi - 21, k - 1] = cents[-1]
        row = table[midi - 21]
        print(f"{ev.note_name(midi):4s} {len(found):3d} partials to k{last}, {len(moved)} off the model's; cents at k10/20/30/40/60/100: " +
              " ".join(f"{row[k - 1]:+6.1f}" for k in (10, 20, 30, 40, 60, 100)))
    # Neighbouring strings drift alike. A key 45 cents off its moved neighbours' median (+-3 keys) at the 30th or 40th
    # partial followed something else, as C#1-E1 do where their longitudinal resonances (13.06 f0 and its multiples)
    # crowd the series.
    moved = [i for i in range(88) if table[i].any()]
    for i in list(moved):
        near = [j for j in moved if j != i and abs(j - i) <= 3]
        if near and np.max(np.abs(table[i, [29, 39]] - np.median(table[near][:, [29, 39]], axis=0))) > 45:
            print(f"{ev.note_name(i + 21):4s} off its neighbours: the model's series")
            table[i] = 0.0
    return np.clip(table, -600, 100)


def write_tuning(table):
    rows = ",\n".join("    {{" + ", ".join(f"{v:.1f}" for v in row) + "}}" for row in table)
    TUNING.write_text(f"""#pragma once

// Generated by tools/fit_piano.py (tune). Do not edit.
#include "model/PianoData.hpp"

namespace pianocraft::pianos::{PIANO.name} {{

// Each key's partials 1-{PARTIALS}, cents on the series the model would otherwise play (its calibrated partials, then
// the stiff-string law), where the recordings hold them. A partial past the table keeps the last one's.
inline constexpr std::array<std::array<float, tuningPartials>, 88> partialCents{{{{
{rows}}}}};

}} // namespace pianocraft::pianos::{PIANO.name}
""")


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
#include "model/PianoData.hpp"

namespace pianocraft::pianos::{PIANO.name} {{

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

}} // namespace pianocraft::pianos::{PIANO.name}
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
    for midi in [m for m in KEYS if all(PIANO.has(m, layer) for layer in (1, 3, 4))]:
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


def even(table):
    """Each partial's per-key values, the running median of 5 keys and then their running mean over 5: a regulated piano.
    Taken note by note, Meta's piano is uneven (E5's octave 7-10 dB over any neighbour's at every velocity, its fundamental
    dropping to a hum where F5's fades; D#5 5 dB loud), and beats left single keys' decay fits far off. The median drops
    one key's quirk; alone, it kept the edge between registers as a step (C#5-E5's octaves 17 dB over F5-G5's, between
    E5 and F5), which the mean ramps. Age adds unevenness on purpose."""
    from scipy.ndimage import median_filter, uniform_filter1d
    return uniform_filter1d(median_filter(table, size=(5, 1), mode="nearest"), 5, axis=0, mode="nearest")


def beats(power):
    """Power averaged over 0.3 s (15 steps of 0.02 s), along the first axis: the envelope through a unison's beats.
    Fitted in dB unaveraged, beats' dips pulled a decay fast and its aftersound low (C6's fundamental: -60 dB/s and
    -19 dB, where its beat-averaged level falls at -21 and holds at -16), or failed it (G#7's, 7.8 dB rms, so the top
    treble took the law: twice too fast). Data and curve are averaged alike."""
    from scipy.ndimage import uniform_filter1d
    return uniform_filter1d(power, 15, axis=0, mode="nearest")


def fit_decays():
    """Each partial's envelope at layer 3, fitted directly: its power, averaged over beats (beats()), as
    A e^-2at + B e^-2bt over its own floor, from 0.03 s (or half the analysis window) to 5 s, in dB with a soft
    loss. A calibrated partial is a coupled unison: a its prompt decay, b its aftersound, B/A its slow mode's level.
    An extended partial is one string mode: one exponential. A fit counts where the partial rises 10 dB over its
    floor (at 20, the bass's upper partials, which ring on 15-20 dB over it for seconds, all took the surface and
    died 30 dB early by 1.6 s) and the fit stays within 4 dB rms; the others take a smooth surface through the counted ones (keys outside
    the split, knots every 6 keys and 1/2 octave). Then each partial is evened over 5 keys (even()). Returns the
    three tables, as in the header."""
    from scipy.optimize import least_squares
    ref = reference()

    def one(midi):
        ks, fs = partials(midi)
        if not PIANO.has(midi, 3):
            return midi, [None] * len(ks)
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
        db = 10 * np.log10(beats(np.sum(np.abs(z) ** 2, axis=0)) + 1e-24)
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
                    return 10 * np.log10(beats(10 ** (l0 / 10) * np.exp(-2 * a * t) +
                                               10 ** ((l0 + rdb) / 10) * np.exp(-2 * min(a, b) * t)) + noise)
                start = [a0, max(least_after, 0.2 * a0), level0, -20.0]
                bounds = ([slow, least_after, level0 - 40, -60.0], [fast, fast, level0 + 40, -3.0])
            else:
                def curve(p):
                    a, l0 = p
                    return 10 * np.log10(beats(10 ** (l0 / 10) * np.exp(-2 * a * t)) + noise)
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
    tables = [even(t) for t in tables]
    return np.clip(tables[0], -3, 3), np.clip(tables[1], -3, 3), np.clip(tables[2], -20, 15)


def fit_spectrum(rounds, resume=False):
    """From flat tables (keeping the strike positions), each round renders every key and layer and refines:
    - the radiation: a smooth surface (knots every 6 keys, 1/6 octave) through layer 3's level differences,
      recording minus model, less their median, so the instrument's loudness stays the engine's. Each partial weighs
      by the square root of its amplitude re the note's strongest (at least 0.05);
    - each key's partials: what is left of its own differences, in full where the recording holds the partial 10 dB
      over its floor (the floor then biases its level by 0.4 dB), fading to none at 6 dB, within +-40 dB, then the
      evened over 5 keys (even()). At 20 dB and +-25 the low bass's partials above 2 kHz, 14-19 dB over the floor and
      needing ~30 dB over the smooth surface, stayed 7-14 dB weak at every velocity;
    - the velocity law: smooth surfaces (knots every 12 keys, uniform weights) through each partial's difference at
      layers 1 and 4 less its difference at layer 3;
    A partial the recording doesn't hold over its floor mustn't sound over that floor: it pulls down to it. The
    smooth fits use the keys outside the split; every key takes its own partials (measured, as their frequencies
    are). The brightening table is the recordings' own: the velocity surfaces through each partial's level change
    from layer 3, re its key's fundamental."""
    strike, spread, *tables, prompt, aftersound, knee = read_header()
    # From flat tables, or with resume from the header's (more rounds where a layer is still converging).
    radiation, partial, velocity, brightening = tables if resume else flat()[:4]
    ref = reference()
    lv, fl = split(ref, "level"), split(ref, "floor")
    key_knots, band_knots, vk = np.arange(21, 115, 6.0), np.arange(0, BANDS + 2, 2.0), np.arange(21, 121, 12.0)

    def held(midi, layer, margin=10):
        return lv[midi, layer] > fl[midi, layer] + margin

    for i, layer in enumerate(FIT_LAYERS):
        rows = []
        for midi in [m for m in ev.FIT_KEYS if (m, layer) in lv]:
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
        diff = {item: lv[item] - mv[item] for item in lv if item[1] in (1, 3, 4)}
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
        for midi in [m for m in KEYS if (m, 3) in diff]:
            ks, fs = partials(midi)
            surface = bilinear(np.full(len(fs), midi), fs, key_knots, band_knots) @ coef
            trust = np.clip((lv[midi, 3] - fl[midi, 3] - 6) / 4, 0, 1)
            left = np.clip(diff[midi, 3] - offset - surface, -40, 40) * trust
            for k, v in zip(ks, left):
                if k <= PARTIALS:
                    partial[midi - 21, k - 1] = np.clip(partial[midi - 21, k - 1] + v, -40, 40)
        partial = even(partial)
        # The velocity law, layers 1 and 4.
        for i, layer in enumerate(FIT_LAYERS):
            rows = []
            for midi in [m for m in ev.FIT_KEYS if (m, layer) in diff]:
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
        for layer in sorted(PIANO.layers):
            errors, early, late, knees = [], [], [], []
            for midi in [m for m in keys if (m, layer) in ref]:
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
    if command == "tune":
        write_tuning(fit_tuning())
        CACHE.unlink(missing_ok=True)  # measured at the old frequencies
        subprocess.run(["cmake", "--build", str(ROOT / "build"), "--target", "pianocraft-render"], check=True,
                       capture_output=True)
    elif command == "measure":
        write_calibration(fit_calibration())
    elif command == "neutral":
        write_header([analytic_strike(m) for m in KEYS], 0.0, *flat(), "neutral")
    elif command == "strike":
        write_header(*fit_strike(), *read_header()[2:], "strike positions")
    elif command == "decays":
        strike, spread, radiation, partial, velocity, brightening, *_ = read_header()
        write_header(strike, spread, radiation, partial, velocity, brightening, *fit_decays(), "decays")
    elif command == "spectrum":
        fit_spectrum(int(args[0]) if args else 3, resume="resume" in args)
    elif command == "score":
        score()
    else:
        sys.exit(__doc__)
