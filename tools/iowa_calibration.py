#!/usr/bin/env python3
"""Calibrate the orchestral voices against the University of Iowa Musical Instrument Samples (model/iowa-provenance.json).

    tools/iowa_calibration.py fetch [instrument]       download, check and convert the recordings (macOS: afconvert);
                                                       piano is the Steinway B the Grand is fitted to (tools/pianos.py)
    tools/iowa_calibration.py reference <instrument>   analyse the recordings: calibration/iowa-mis/<instrument>/reference.json
    tools/iowa_calibration.py score <instrument>       render the model at the same notes and dynamics, and compare
    tools/iowa_calibration.py fit <brass>              fit a brass bell's high-pass and tone low-pass to the recordings
    tools/iowa_calibration.py modes <crotales|bells|vibraphone>
                                                       struck metal: fit each mode's ratio, level and decay; crotales and
                                                       vibraphone also score the model

<instrument> is horn, trumpet, trombone, tuba, flute, oboe, clarinet, violin, viola, cello or bass; the bells stand in
for the celesta's bars, which the collection lacks. The recordings are analysis inputs only:
they stay in calibration/ (git-ignored) and nothing of them reaches the plugin but fitted constants.
Each note is compared by its harmonics' levels (shape, weighted to the strong ones), its level, its brightness (the
share above 3 kHz), its attack (to within 10 dB of its steady level), the noise between its harmonics and how steady
its level is. The model plays pp, mf and ff as CC1 0.25, 0.6 and 1.0.
"""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.optimize import least_squares

ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / "calibration" / "iowa-mis"
PROVENANCE = ROOT / "model" / "iowa-provenance.json"
ENGINE = ROOT / "src" / "engine" / "BrassEngine.cpp"
RENDERER = ROOT / "build" / "pianocraft-render-voice"
DYNAMICS = {"pp": (0.25, 0.35), "mf": (0.6, 0.6), "ff": (1.0, 0.9)}  # CC1, velocity
# How each corpus splits into notes, the model's compass, which harmonics are above its noise (dB re the
# strongest, and below what frequency: the tuba was recorded quietly), and how the model is rendered to match
# (the brass at Stereo Width 0; the flute without vibrato, as recorded).
INSTRUMENTS = {
    "horn": {"prefix": "Horn", "split": "dips", "keys": (34, 77), "floor": -65, "fmax": 8000, "render": ["4=0"]},
    "trumpet": {"prefix": "Trumpet", "split": "regions", "keys": (52, 84), "floor": -65, "fmax": 8000, "render": ["4=0"]},
    "trombone": {"prefix": "Trombone", "split": "regions", "keys": (40, 77), "floor": -65, "fmax": 8000, "render": ["4=0"]},
    "tuba": {"prefix": "Tuba", "split": "regions", "keys": (26, 65), "floor": -45, "fmax": 3000, "render": ["4=0"]},
    "flute": {"prefix": "Flute", "split": "regions", "keys": (59, 98), "floor": -65, "fmax": 10000, "render": ["3=0"]},
    "oboe": {"prefix": "Oboe", "split": "regions", "keys": (58, 93), "floor": -65, "fmax": 10000, "render": []},
    "clarinet": {"prefix": "Clarinet", "split": "regions", "keys": (50, 94), "floor": -65, "fmax": 10000, "render": []},
    # The strings plugin plays each register on one body: basses below C2, cellos below C3, violas below G3,
    # violins above. Each recording is compared over its own register, as one unmuted player (Section 1, Con
    # Sordino off, Stereo Width 0).
    "violin": {"prefix": "Violin", "split": "pitched", "keys": (55, 100), "floor": -65, "fmax": 10000, "renderer": "strings", "render": ["2=1", "3=0", "7=0", "11=0"]},
    "viola": {"prefix": "Viola", "split": "pitched", "keys": (48, 54), "floor": -65, "fmax": 10000, "renderer": "strings", "render": ["2=1", "3=0", "7=0", "11=0"]},
    "cello": {"prefix": "Cello", "split": "pitched", "keys": (36, 47), "floor": -65, "fmax": 10000, "renderer": "strings", "render": ["2=1", "3=0", "7=0", "11=0"]},
    "bass": {"prefix": "Bass", "split": "pitched", "keys": (28, 35), "floor": -65, "fmax": 10000, "renderer": "strings", "render": ["2=1", "3=0", "7=0", "11=0"]},
}
BRASS = ("horn", "trumpet", "trombone", "tuba")
NAMES = {"C": 0, "Db": 1, "D": 2, "Eb": 3, "E": 4, "F": 5, "Gb": 6, "G": 7, "Ab": 8, "A": 9, "Bb": 10, "B": 11}


def midi(name: str) -> int:
    m = re.fullmatch(r"([A-G]b?)(\d)", name)
    return NAMES[m.group(1)] + 12 * (int(m.group(2)) + 1)


def frames_db(x, rate, hop=0.01):
    n = int(hop * rate)
    k = len(x) // n
    return 20 * np.log10(np.sqrt(np.mean(x[:k * n].reshape(k, n) ** 2, axis=1) + 1e-20))


def dips(x, rate, count):
    """A chromatic run played without breaks: split at its count-1 deepest dips, each note then trimmed to within
    40 dB of its own peak."""
    db = frames_db(x, rate)
    smooth = np.convolve(db, np.ones(3) / 3, mode="same")
    minima = sorted((i for i in range(1, len(smooth) - 1) if smooth[i] <= smooth[i - 1] and smooth[i] <= smooth[i + 1]),
                    key=lambda i: smooth[i])
    cuts = []
    for i in minima:
        if len(cuts) == count - 1:
            break
        if all(abs(i - c) > 80 for c in cuts) and 50 < i < len(smooth) - 50:
            cuts.append(i)
    edges = [0] + sorted(cuts) + [len(db)]
    out = []
    for a, b in zip(edges, edges[1:]):
        on = np.nonzero(db[a:b] > db[a:b].max() - 40)[0]
        out.append(((a + on[0]) * 0.01, (a + on[-1] + 1) * 0.01))
    return out


def regions(x, rate, count):
    """Notes separated by silence or a noise floor: the threshold (10-45 dB under the top) giving exactly count notes
    of 0.4 s or more, each extended while it stays above the floor and within 50 dB of its peak."""
    db = frames_db(x, rate)
    top = np.percentile(db, 99)
    for drop in range(10, 46):
        found, start = [], None
        for i, v in enumerate(np.append(db > top - drop, False)):
            if v and start is None:
                start = i
            if not v and start is not None:
                if found and start - found[-1][1] < 4:
                    found[-1] = (found[-1][0], i)
                else:
                    found.append((start, i))
                start = None
        found = [r for r in found if r[1] - r[0] >= 40]
        if len(found) == count:
            break
    else:
        raise RuntimeError(f"no threshold gives {count} notes")
    floor = max(np.percentile(db[db > -150], 10), top - 90)
    out = []
    for a, b in found:
        edge = max(floor + 10, db[a:b].max() - 50)
        while a > 0 and db[a - 1] > edge:
            a -= 1
        while b < len(db) and db[b] > edge:
            b += 1
        out.append((a * 0.01, b * 0.01))
    return out


def pitched(x, rate, keys):
    """Notes apart, not always the file name's whole chromatic span (the string runs skip some): every region
    30 dB or less under the loudest, of 0.4 s or more, each labelled by its own pitch (YIN, de Cheveigne and
    Kawahara 2002), kept if within 40 cents of a key in the span. Returns (key, start, end)."""
    db = frames_db(x, rate)
    top = np.percentile(db, 99)
    loud = np.append(db > top - 30, False)
    found, start = [], None
    for i, v in enumerate(loud):
        if v and start is None:
            start = i
        if not v and start is not None:
            if found and start - found[-1][1] < 4:
                found[-1] = (found[-1][0], i)
            else:
                found.append((start, i))
            start = None
    out = []
    for a, b in (r for r in found if r[1] - r[0] >= 40):
        mid = (a + b) // 2 * int(0.01 * rate)
        frame = x[mid:mid + int(0.1 * rate)]
        lo, hi = int(rate / 2500), int(rate / 35)
        w = len(frame) - hi
        d = np.array([np.sum((frame[:w] - frame[t:t + w]) ** 2) for t in range(1, hi)])
        cm = d * np.arange(1, hi) / np.maximum(np.cumsum(d), 1e-30)
        below = np.nonzero(cm[lo:] < 0.15)[0]
        t = lo + (below[0] if below.size else np.argmin(cm[lo:]))
        while t + 1 < len(cm) and cm[t + 1] < cm[t]:
            t += 1
        f0 = rate / (t + 1)
        key = round(69 + 12 * np.log2(f0 / 440))
        if key in keys and abs(1200 * np.log2(f0 / (440 * 2 ** ((key - 69) / 12)))) < 40:
            out.append((key, a * 0.01, b * 0.01))
    return out


def analyse(x, rate, start, end, key, count=20):
    note = x[int(start * rate):int(end * rate)]
    steady = note[int(0.35 * len(note)):int(0.8 * len(note))]
    level = 20 * np.log10(np.sqrt(np.mean(steady ** 2)) + 1e-20)
    spec = np.abs(np.fft.rfft(steady * np.hanning(len(steady)))) ** 2
    freqs = np.fft.rfftfreq(len(steady), 1 / rate)
    nominal = 440 * 2 ** ((key - 69) / 12)
    near = (freqs > nominal * 0.96) & (freqs < nominal * 1.04)
    f0 = freqs[near][np.argmax(spec[near])]
    h = []
    for k in range(1, count + 1):
        sel = (freqs > (k - 0.3) * f0) & (freqs < (k + 0.3) * f0)
        h.append(spec[sel].max() if k * f0 < rate / 2 * 0.95 and sel.any() else 0.0)
    h = np.array(h)
    rel = (freqs / f0) % 1.0
    between = (np.minimum(rel, 1 - rel) > 0.3) & (freqs > f0 * 0.7) & (freqs < 8000)
    db = frames_db(note, rate, 0.005)
    attack = next((i for i, v in enumerate(db) if v > level - 10), -1) * 0.005
    return {"key": key, "f0": float(f0), "level": float(level),
            "h": [round(float(v), 1) for v in 10 * np.log10(h / h.max() + 1e-12)],
            "above3k": float(10 * np.log10(spec[freqs > 3000].sum() / spec.sum() + 1e-30)),
            "noise": float(10 * np.log10(spec[between].mean() * between.size / max(between.sum(), 1) / spec.sum() + 1e-30)),
            "attack": attack, "unsteady": float(np.std(frames_db(steady, rate, 0.02)))}


def read(path):
    rate, x = wavfile.read(path)
    x = x.astype(float) / (32768.0 if x.dtype == np.int16 else 2.0 ** 31)
    return rate, x.mean(axis=1) if x.ndim > 1 else x


def fetch(only=None):
    record = json.loads(PROVENANCE.read_text())
    for source in record["sources"]:
        if only is not None and source["instrument"] != only:
            continue
        folder = CORPUS / source["instrument"]
        folder.mkdir(parents=True, exist_ok=True)
        if "files" in source:  # the strings: a file per string and range, no archive
            fetch_files(source, folder)
            continue
        archive = folder / source["archive"]
        if not archive.exists():
            urllib.request.urlretrieve(source["url"], archive)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if digest != source["sha256"]:
            sys.exit(f"{archive}: sha256 {digest}, the provenance record says {source['sha256']}")
        (folder / "wav").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory() as unpacked, zipfile.ZipFile(archive) as z:
            z.extractall(unpacked)
            for aiff in Path(unpacked).rglob("*.aif*"):
                named = re.search(r"\.(pp|mf|ff)\.\s*([A-G][b0-9A-G]*)", aiff.name)
                if named is None:
                    continue  # the clarinet's recording of the empty room
                dyn, span = named.groups()
                out = folder / "wav" / f"{INSTRUMENTS[source['instrument']]['prefix']}.{dyn}.{span}.wav"
                subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16", str(aiff), str(out)], check=True)
        print(f"{source['instrument']}: {archive.name} verified, converted to {folder / 'wav'}")


def fetch_files(source, folder):
    (folder / "aiff").mkdir(exist_ok=True)
    (folder / "wav").mkdir(exist_ok=True)
    for entry in source["files"]:
        aiff = folder / "aiff" / Path(urllib.parse.unquote(entry["url"])).name
        if not aiff.exists():
            urllib.request.urlretrieve(entry["url"], aiff)
        digest = hashlib.sha256(aiff.read_bytes()).hexdigest()
        if digest != entry["sha256"]:
            sys.exit(f"{aiff}: sha256 {digest}, the provenance record says {entry['sha256']}")
        if source["instrument"] == "piano":  # the Steinway B: PianoCraft's Grand (tools/pianos.py), named as Iowa names it
            subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16", str(aiff), str(folder / "wav" / f"{aiff.stem}.wav")], check=True)
            continue
        if source["instrument"] in STRUCK:  # 24-bit, decays to the noise: kept at 32 bits
            subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI32", str(aiff), str(folder / "wav" / f"{aiff.stem}.wav")], check=True)
            continue
        dyn = re.search(r"\.(pp|mf|ff)\.", aiff.name).group(1)
        string = re.search(r"sul([A-G])", aiff.name).group(1)
        span = re.search(r"\.([A-G]b?\d(?:[A-G]b?\d)?)\.aif", aiff.name).group(1)
        out = folder / "wav" / f"{INSTRUMENTS[source['instrument']]['prefix']}.{dyn}.{span}-sul{string}.wav"
        subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16", str(aiff), str(out)], check=True)
    print(f"{source['instrument']}: {len(source['files'])} files verified, converted to {folder / 'wav'}")


def reference(brass):
    spec = INSTRUMENTS[brass]
    results = {}
    for path in sorted((CORPUS / brass / "wav").glob(f"{spec['prefix']}.*.wav")):
        _, dyn, span = path.stem.split(".")
        names = re.findall(r"[A-G]b?\d", span)
        keys = list(range(midi(names[0]), midi(names[-1]) + 1))
        rate, x = read(path)
        if spec["split"] == "pitched":
            labelled = pitched(x, rate, keys)
        else:
            try:
                notes = (dips if spec["split"] == "dips" else regions)(x, rate, len(keys))
            except RuntimeError:  # a run played without breaks
                notes = dips(x, rate, len(keys))
            labelled = [(key, a, b) for key, (a, b) in zip(keys, notes)]
        for key, a, b in labelled:
            if b - a < 0.3:  # a split that missed its note
                continue
            note = analyse(x, rate, a, b, key)
            in_tune = abs(note["f0"] / (440 * 2 ** ((key - 69) / 12)) - 1) < 0.025  # else the split slipped a note
            if note["level"] > -150 and all(np.isfinite(note["h"])) and in_tune:
                results.setdefault(dyn, []).append(note)
    out = CORPUS / brass / "reference.json"
    out.write_text(json.dumps(results, indent=1))
    print(f"wrote {out}: " + ", ".join(f"{len(v)} {k}" for k, v in results.items()))


def model_note(brass, key, dyn, folder):
    cc1, velocity = DYNAMICS[dyn]
    out = Path(folder) / f"{brass}-{key}-{dyn}.wav"
    spec = INSTRUMENTS[brass]
    subprocess.run([str(RENDERER), spec.get("renderer", brass), str(out), str(key), str(velocity), "2.8", "2.5", f"cc1={cc1}", *spec["render"]],
                   check=True, capture_output=True)
    rate, x = read(out)
    (a, b), = dips(x, rate, 1)
    return analyse(x, rate, a, b, key)


def pairs(brass):
    ref = json.loads((CORPUS / brass / "reference.json").read_text())
    low, high = INSTRUMENTS[brass]["keys"]
    jobs = [(dyn, r) for dyn in DYNAMICS for r in ref.get(dyn, []) if low <= r["key"] <= high]
    unique = sorted({(dyn, r["key"]) for dyn, r in jobs})  # a note recorded on two strings is rendered once
    with tempfile.TemporaryDirectory() as folder, ThreadPoolExecutor(8) as pool:
        models = dict(zip(unique, pool.map(lambda j: model_note(brass, j[1], j[0], folder), unique)))
    return [(dyn, r, models[dyn, r["key"]]) for dyn, r in jobs]


def shape_error(rh, mh, f, spec):
    """Both spectra clipped at the recordings' floor: a harmonic the model sounds where the recording has only
    noise counts by how far it stands above that noise."""
    ok = (f < spec["fmax"]) & (np.maximum(rh, mh) > spec["floor"])
    rh, mh = np.maximum(rh, spec["floor"]), np.maximum(mh, spec["floor"])
    d, w = (rh - mh)[ok], 10 ** (np.maximum(rh, mh)[ok] / 20)
    d = d - np.sum(d * w) / np.sum(w)
    return float(np.sqrt(np.sum(w * d ** 2) / np.sum(w)))


def score(brass):
    spec = INSTRUMENTS[brass]
    rows = pairs(brass)
    offset = np.median([m["level"] - r["level"] for _, r, m in rows])
    for dyn in DYNAMICS:
        sel = [(r, m) for d, r, m in rows if d == dyn]
        if not sel:
            continue
        n = lambda r, m: min(len(r["h"]), len(m["h"]))
        errors = [shape_error(np.array(r["h"][:n(r, m)]), np.array(m["h"][:n(r, m)]), np.arange(1, n(r, m) + 1) * r["f0"], spec)
                  for r, m in sel]
        median = lambda key, scale=1.0: np.median([(m[key] - r[key]) * scale for r, m in sel])
        print(f"{brass} {dyn}: harmonics {np.mean(errors):.1f} dB, level {np.mean([m['level'] - r['level'] - offset for r, m in sel]):+.1f} dB, "
              f"above 3 kHz {median('above3k'):+.1f} dB, attack {median('attack', 1000):+.0f} ms, noise {median('noise'):+.1f} dB, "
              f"unsteadiness {np.median([m['unsteady'] for r, m in sel]):.2f} (recordings {np.median([r['unsteady'] for r, m in sel]):.2f}) dB")


# The model's output filters, as fitted: the bell's Butterworth high-pass and the tone's three one-pole low-passes.
def one_pole_hp(f, fc): return 10 * np.log10(f ** 2 / (f ** 2 + fc ** 2))
def one_pole_lp(f, fc): return -10 * np.log10(1 + (f / fc) ** 2)
def section_hp(f, fc, q): return 10 * np.log10((f / fc) ** 4 / ((1 - (f / fc) ** 2) ** 2 + (f / fc / q) ** 2))
def bell(f, fc, order): return {1: one_pole_hp(f, fc), 2: section_hp(f, fc, 0.7071), 3: one_pole_hp(f, fc) + section_hp(f, fc, 1.0)}[order]
def tone(f, b, f0, corner, bloom, track): return 3 * one_pole_lp(f, corner * (1 + bloom * b ** 3) * (f0 / 130.81) ** track)


def fit(brass):
    """The model's spectrum less its current filters is what its bore radiates; fit the filters that best turn that
    into the recordings', for a third- and a second-order bell, with and without pitch tracking."""
    spec = INSTRUMENTS[brass]
    fields = re.search(rf"constexpr BrassModel {brass}\{{([^{{]*)", ENGINE.read_text()).group(1).split(",")
    now = dict(fc=float(fields[3]), order=int(fields[4]), corner=float(fields[5]), bloom=float(fields[6]), track=float(fields[7]))
    rows = []
    for dyn, r, m in pairs(brass):
        n = min(len(r["h"]), len(m["h"]))
        f = np.arange(1, n + 1) * r["f0"]
        rh, mh = np.array(r["h"][:n]), np.array(m["h"][:n])
        ok = (f < spec["fmax"]) & (rh > spec["floor"])
        b = DYNAMICS[dyn][0]
        radiated = mh - bell(f, now["fc"], now["order"]) - tone(f, b, r["f0"], now["corner"], now["bloom"], now["track"])
        rows.append((dyn, b, r["f0"], f[ok], rh[ok], radiated[ok]))

    def errors(filt, weight=lambda x: 10 ** (x / 20)):
        out = []
        for _, b, f0, f, rh, radiated in rows:
            pred = radiated + filt(f, b, f0)
            d, w = rh - pred, weight(np.maximum(rh, pred))
            out.append((d - np.sum(d * w) / np.sum(w)) * np.sqrt(w))
        return np.concatenate(out)

    def summary(filt):
        per = {d: [] for d in DYNAMICS}
        for dyn, b, f0, f, rh, radiated in rows:
            per[dyn].append(shape_error(rh, radiated + filt(f, b, f0), f, {"fmax": 1e9, "floor": -1e9}))
        return "  ".join(f"{d} {np.mean(v):.1f}" for d, v in per.items() if v)

    print("current:", summary(lambda f, b, f0: bell(f, now["fc"], now["order"]) + tone(f, b, f0, now["corner"], now["bloom"], now["track"])))
    for order in (3, 2):
        for tracked in (False, True):
            def filt_of(p):
                fc, corner, bloom = np.exp(p[:3])
                return lambda f, b, f0: bell(f, fc, order) + tone(f, b, f0, corner, bloom, p[3] if tracked else 0.0)
            x0 = [np.log(now["fc"]), np.log(now["corner"]), np.log(max(now["bloom"], 0.1)), 0.0]
            p = least_squares(lambda p: errors(filt_of(p), lambda x: np.sqrt(10 ** (x / 20))), x0,
                              bounds=([np.log(30), np.log(100), np.log(0.01), -1.5 if tracked else -1e-9],
                                      [np.log(5000), np.log(20000), np.log(30), 1.5 if tracked else 1e-9])).x
            fc, corner, bloom = np.exp(p[:3])
            print(f"bell order {order} at {fc:.0f} Hz, tone {corner:.0f} Hz x (1 + {bloom:.2f} b^3) x (f0/C3)^{p[3] if tracked else 0:+.2f}:",
                  summary(filt_of(p)))


# Struck metal has no harmonics: each stroke is read as modes, each with its frequency ratio to the fundamental, its
# level at the strike (dB re the fundamental, extrapolated to the onset) and its T60. The crotales' five mode families
# close in up the range (the trend below); the bells' partials vary from bar to bar, so their strongest are binned.
STRUCK = {"crotales": {"keys": (84, 108), "velocity": {"pp": 0.35, "ff": 0.9},
                       "trend": lambda k: [1.0, 2.14 - 0.0098 * (k - 84), 3.74 - 0.0233 * (k - 84), 5.70 - 0.0425 * (k - 84),
                                           7.98 - 0.067 * (k - 84), 10.7 - 0.107 * (k - 84), 13.8 - 0.15 * (k - 84)]},
          "bells": {"keys": (72, 112), "bins": [(2.9, 3.4), (6.0, 7.9), (9.0, 11.5), (13.0, 17.0)]},
          # Aluminium bars tuned 1:4:10, a torsional mode falling through them and the 4th bending mode (both traced
          # while clear of the tuned ones), x octaves from C6. The exact 2nd and 3rd harmonics, 40 dB down and dying
          # at 2 and 3 times the fundamental's rate, are distortion, not modes.
          "vibraphone": {"keys": (48, 89), "velocity": {"pp": 0.35, "mf": 0.6, "ff": 0.9},
                         "families": [(lambda x: 4.0, 89), (lambda x: 10.0, 89), (lambda x: 2.05 - 2.6 * x, 70),
                                      (lambda x: 9.1 - 2.3 * x, 64)]}}


def peak(f, db, lo, hi):
    sel = np.nonzero((f > lo) & (f < hi))[0]
    if sel.size == 0:
        return None
    i = sel[np.argmax(db[sel])]
    a, b, c = db[i - 1], db[i], db[i + 1]
    d = 0.5 * (a - c) / (a - 2 * b + c)
    return f[i] + d * (f[1] - f[0]), b - 0.25 * (a - c) * d


def decay(seg, rate, f):
    """A mode's level at the strike and its T60: its 80 ms level every 20 ms, fitted while 12 dB over the noise and
    within 45 dB of its start."""
    hop, m = int(0.02 * rate), int(0.08 * rate)
    frames = min((len(seg) - m) // hop, 1500)
    k = np.exp(-2j * np.pi * f * np.arange(m) / rate) * np.hanning(m)
    track = 20 * np.log10(np.abs(np.array([np.dot(seg[j * hop:j * hop + m], k) for j in range(frames)])) * 2 / np.hanning(m).sum() + 1e-20)
    above = track > max(np.percentile(track, 3) + 12, track[:5].max() - 45)
    stop = int(np.argmax(~above[2:]) + 2) if (~above[2:]).any() else frames
    if stop < 6:
        return track[:5].max(), np.nan
    slope, start = np.polyfit(np.arange(2, stop) * hop / rate, track[2:stop], 1)
    return start, (-60 / slope if slope < 0 else np.nan)


def stroke(seg, rate, key, ratios=None):
    seg = seg[int(np.argmax(np.abs(seg[:rate]) > np.abs(seg[:rate]).max() * 0.1)):]
    n = rate
    window = np.pad(seg[int(0.005 * rate):int(0.005 * rate) + n], (0, max(0, n - len(seg))))[:n]
    db = 20 * np.log10(np.abs(np.fft.rfft(window * np.hanning(n), 4 * n)) + 1e-20)
    f = np.fft.rfftfreq(4 * n, 1 / rate)
    f1, _ = peak(f, db, 440 * 2 ** ((key - 69) / 12) * 0.96, 440 * 2 ** ((key - 69) / 12) * 1.04)
    level, t60 = decay(seg, rate, f1)
    if ratios is None:  # the strongest peaks above 1.2 f1, within 50 dB of it
        from scipy.signal import find_peaks
        top = db[(f > f1 * 0.98) & (f < f1 * 1.02)].max()
        found, _ = find_peaks(db, height=top - 50, distance=int(f1 * 0.15 / (f[1] - f[0])))
        found = found[(f[found] > 1.2 * f1) & (f[found] < 0.45 * rate)]
        ratios = sorted(f[found[np.argsort(db[found])[::-1][:6]]] / f1)
        width = 0.01
    else:
        ratios, width = ratios[1:], 0.07
    modes = []
    for r in ratios:
        found = peak(f, db, f1 * r * (1 - width), f1 * r * (1 + width))
        if found is not None and found[0] < 0.45 * rate:
            start, t = decay(seg, rate, found[0])
            modes.append({"ratio": found[0] / f1, "level": start - level, "t60": t})
    return {"key": key, "f1": f1, "level": level, "t60": t60, "modes": modes}


def strokes(x, rate):
    """Strokes in a run: a rise of 20 dB within 30 ms in the high-passed level, at least 2 s apart."""
    from scipy.signal import butter, sosfilt
    y = sosfilt(butter(4, 400, "highpass", fs=rate, output="sos"), x)
    hop = int(0.005 * rate)
    db = frames_db(y, rate, 0.005)
    top = np.percentile(db, 99.5)
    found = []
    for i in range(6, len(db)):
        if db[i] > top - 40 and db[i] - db[i - 6:i].min() > 20 and (not found or i - found[-1] > 400):
            found.append(i)
    return [i * hop for i in found]


def struck_reference(name):
    rows = []
    for path in sorted((CORPUS / name / "wav").glob("*.wav")):
        rate, x = read(path)
        if name == "crotales":
            note, dyn = re.search(r"\.([A-G]b?\d)\.(pp|ff)", path.name).groups()
            rows.append({**stroke(x, rate, midi(note), STRUCK[name]["trend"](midi(note))), "dyn": dyn})
            continue
        dyn, span = re.search(r"\.(pp|mf|ff)\.(\w+)\.wav", path.name).groups()
        names = re.findall(r"[A-G]b?\d", span)
        keys = list(range(midi(names[0]), midi(names[1]) + 1))
        starts = strokes(x, rate)
        if len(starts) != len(keys):
            continue  # a stray knock: this run's strokes can't be told apart
        families = STRUCK[name].get("families")
        for key, a, b in zip(keys, starts, starts[1:] + [len(x)]):
            ratios = None if families is None else [1.0] + [law((key - 84) / 12) for law, top in families if key <= top]
            rows.append({**stroke(x[max(a - int(0.05 * rate), 0):b], rate, key, ratios), "dyn": dyn})
    return [r for r in rows if np.isfinite(r["t60"])]


def struck_laws(name, rows):
    ff = [r for r in rows if r["dyn"] == ("ff" if name == "crotales" else "mf")]
    x = np.array([(r["key"] - 84) / 12 for r in ff])
    t = np.log([r["t60"] for r in ff])
    fit = np.polyfit(x, t, 1 if name == "crotales" else 2)
    print(f"{name}: fundamental T60 = exp({' + '.join(f'{c:.3f} x^{len(fit) - 1 - i}' for i, c in enumerate(fit))}), x octaves from C6, "
          f"scatter x{np.exp(np.std(t - np.polyval(fit, x))):.2f}")
    by = {(r["key"], r["dyn"]): r for r in rows}
    for loud, soft in (("ff", "pp"), ("ff", "mf"), ("mf", "pp")):
        d = [by[k, loud]["level"] - by[k, soft]["level"] for k, dd in by if dd == loud and (k, soft) in by]
        if d:
            print(f"  fundamental {loud} over {soft}: median {np.median(d):.1f} dB ({len(d)} notes)")
    if name == "crotales":
        for n in range(6):
            sel = [(xi, r["modes"][n]) for xi, r in zip(x, ff) if len(r["modes"]) > n]
            xs = np.array([a for a, _ in sel])
            ratio = np.polyfit(xs, [m["ratio"] for _, m in sel], 1)
            level = np.polyfit(xs, [m["level"] for _, m in sel], 1)
            ok = [(a, m["t60"]) for a, m in sel if np.isfinite(m["t60"])]
            life = np.polyfit([a for a, _ in ok], np.log([v for _, v in ok]), 1)
            print(f"  mode {n + 2}: ratio {ratio[1]:.3f} {ratio[0]:+.3f} x, ff level {level[1]:+.1f} {level[0]:+.1f} x dB, "
                  f"T60 exp({life[1]:.3f} {life[0]:+.3f} x), in {len(sel)} discs")
        print(f"  fundamental ff level {np.polyfit(x, [r['level'] for r in ff], 1)[0]:+.1f} dB an octave")
        drops = []
        for r in rows:
            if r["dyn"] == "ff" and (r["key"], "pp") in by:
                q = by[r["key"], "pp"]
                drops.append((r["f1"], r["level"] - q["level"]))
                drops += [(r["f1"] * a["ratio"], r["level"] - q["level"] + a["level"] - b["level"]) for a, b in zip(r["modes"], q["modes"])]
        drops = np.array(drops)
        law = np.polyfit(np.log2(drops[:, 0] / 1000), drops[:, 1], 1)
        print(f"  pp to ff: {law[1]:.1f} + {law[0]:.2f} log2(f / 1 kHz) dB, over {len(drops)} modes")
    else:
        for lo, hi in STRUCK[name]["bins"]:
            found = [m for r in ff for m in r["modes"] if lo <= m["ratio"] < hi]
            t60 = [m["t60"] for m in found if np.isfinite(m["t60"])]
            print(f"  partials {lo}-{hi}: in {len(found)} of {len(ff)} bars, median ratio {np.median([m['ratio'] for m in found]):.2f}, "
                  f"level {np.median([m['level'] for m in found]):+.0f} dB, T60 {np.median(t60):.1f} s")


def vibraphone_laws(rows):
    """The vibraphone's laws: each family's ratio, ff level and T60 against x, and from pp to ff how much its level
    re the fundamental falls an octave above it (the yarn's softening)."""
    families = STRUCK["vibraphone"]["families"]
    def family(r):
        x = (r["key"] - 84) / 12
        out = {}
        for m in (m for m in r["modes"] if np.isfinite(m["t60"]) and m["level"] > -50):  # a mode, not the window's noise
            n = int(np.argmin([abs(np.log(m["ratio"] / law(x))) if r["key"] <= top else np.inf for law, top in families]))
            if abs(np.log(m["ratio"] / families[n][0](x))) < 0.07 and n not in out:
                out[n] = m
        return out
    by = {(r["key"], r["dyn"]): r for r in rows}
    ff = [r for r in rows if r["dyn"] == "ff"]
    x = np.array([(r["key"] - 84) / 12 for r in ff])
    t = np.log([r["t60"] for r in ff])
    fit = np.polyfit(x, t, 2)
    print(f"vibraphone: fundamental T60 = exp({fit[2]:.3f} {fit[1]:+.3f} x {fit[0]:+.3f} x^2), scatter x{np.exp(np.std(t - np.polyval(fit, x))):.2f}; "
          f"ff level {np.polyfit(x, [r['level'] for r in ff], 1)[0]:+.1f} dB an octave")
    for n, (law, top) in enumerate(families):
        sel = [((r["key"] - 84) / 12, family(r)[n]) for r in ff if n in family(r)]
        xs = np.array([a for a, _ in sel])
        ratio = np.polyfit(xs, [m["ratio"] for _, m in sel], 1)
        level = np.polyfit(xs, [m["level"] for _, m in sel], 1)
        spread = np.std([m["level"] - np.polyval(level, a) for a, m in sel])
        ok = [(a, m["t60"]) for a, m in sel if np.isfinite(m["t60"])]
        life = np.polyfit([a for a, _ in ok], np.log([v for _, v in ok]), 1)
        print(f"  mode {n + 2}: ratio {ratio[1]:.3f} {ratio[0]:+.3f} x, ff level {level[1]:+.1f} {level[0]:+.1f} x dB (+-{spread:.1f}), "
              f"T60 exp({life[1]:.3f} {life[0]:+.3f} x), in {len(sel)} of {len(ff)} bars")
    for soft in ("mf", "pp"):
        fund = [by[r["key"], "ff"]["level"] - r["level"] for r in rows if r["dyn"] == soft and (r["key"], "ff") in by]
        tilt = [(np.log2(a["ratio"]), (family(by[r["key"], "ff"])[n]["level"] - a["level"]))
                for r in rows if r["dyn"] == soft and (r["key"], "ff") in by
                for n, a in family(r).items() if n in family(by[r["key"], "ff"])]
        o, d = np.array(tilt).T
        print(f"  ff over {soft}: fundamental {np.median(fund):.1f} dB ({len(fund)} bars); upper modes a further "
              f"{np.sum(o * d) / np.sum(o * o):.1f} dB an octave above it ({len(d)} modes)")


def whole(x, rate):
    """A stroke's level over its first 0.3 s and its share above 3 kHz, dB."""
    from scipy.signal import butter, sosfilt
    x = x[int(np.argmax(np.abs(x) > np.abs(x).max() * 0.1)):][:int(0.3 * rate)]
    high = sosfilt(butter(4, 3000, "highpass", fs=rate, output="sos"), x)
    total = 10 * np.log10(np.mean(x ** 2) + 1e-30)
    return total, 10 * np.log10(np.mean(high ** 2) + 1e-30) - total


def modes(name):
    rows = struck_reference(name)
    if name == "vibraphone":
        return vibraphone_laws(rows)
    struck_laws(name, rows)
    if name != "crotales":
        return
    def render(job):
        key, dyn = job
        out = Path(folder) / f"{key}-{dyn}.wav"
        subprocess.run([str(RENDERER), "crotales", str(out), str(key), str(STRUCK[name]["velocity"][dyn]), "8", "8", "5=0"],
                       check=True, capture_output=True)
        rate, x = read(out)
        return {**stroke(x, rate, key, STRUCK[name]["trend"](key)), "dyn": dyn, "whole": whole(x, rate)}
    with tempfile.TemporaryDirectory() as folder, ThreadPoolExecutor(8) as pool:
        model = {(m["key"], m["dyn"]): m for m in pool.map(render, [(r["key"], r["dyn"]) for r in rows])}
    for dyn in ("pp", "ff"):
        pairs_ = [(r, model[r["key"], dyn]) for r in rows if r["dyn"] == dyn]
        line = []
        for n in range(6):
            sel = [(r["modes"][n], m["modes"][n]) for r, m in pairs_ if len(r["modes"]) > n and len(m["modes"]) > n]
            if not sel:
                continue
            level = np.mean([abs(b["level"] - a["level"]) for a, b in sel])
            life = [np.log(b["t60"] / a["t60"]) for a, b in sel if np.isfinite(a["t60"]) and np.isfinite(b["t60"])]
            line.append(f"mode {n + 2} level {level:.1f} dB, T60 x{np.exp(np.mean(life)):.2f}")
        life = np.exp(np.mean([np.log(m["t60"] / r["t60"]) for r, m in pairs_ if np.isfinite(m["t60"])]))
        print(f"model {dyn} ({len(pairs_)} notes): fundamental T60 x{life:.2f}; " + "; ".join(line))
    recorded = {}
    for path in (CORPUS / name / "wav").glob("*.wav"):
        note, dyn = re.search(r"\.([A-G]b?\d)\.(pp|ff)", path.name).groups()
        rate, x = read(path)
        recorded[midi(note), dyn] = whole(x, rate)
    keys = [k for k in range(84, 109) if (k, "pp") in model and (k, "ff") in model and (k, "pp") in recorded]
    louder = lambda w: [w[k, "ff"][0] - w[k, "pp"][0] for k in keys]
    brighter = lambda w: [w[k, "ff"][1] - w[k, "pp"][1] for k in keys]
    mw = {k: m["whole"] for k, m in model.items()}
    print(f"ff over pp ({len(keys)} notes): recordings {np.median(louder(recorded)):.1f} dB louder, {np.median(brighter(recorded)):+.1f} dB "
          f"brighter; model {np.median(louder(mw)):.1f}, {np.median(brighter(mw)):+.1f}")
    for dyn in ("pp", "ff"):
        share = lambda w: np.median([w[k, dyn][1] for k in keys])
        print(f"  {dyn} above 3 kHz: recordings {share(recorded):+.1f} dB, model {share(mw):+.1f} dB")


if __name__ == "__main__":
    command, *args = sys.argv[1:] or ["help"]
    if command == "fetch" and len(args) <= 1:
        fetch(*args)
    elif command == "modes" and len(args) == 1 and args[0] in STRUCK:
        modes(args[0])
    elif command in ("reference", "score") and len(args) == 1 and args[0] in INSTRUMENTS or command == "fit" and args and args[0] in BRASS:
        {"reference": reference, "score": score, "fit": fit}[command](args[0])
    else:
        sys.exit(__doc__)
