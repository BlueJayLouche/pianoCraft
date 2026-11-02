#!/usr/bin/env python3
"""Calibrate the orchestral voices against the University of Iowa Musical Instrument Samples (model/iowa-provenance.json).

    tools/iowa_calibration.py fetch                    download, check and convert the recordings (macOS: afconvert)
    tools/iowa_calibration.py reference <instrument>   analyse the recordings: calibration/iowa-mis/<instrument>/reference.json
    tools/iowa_calibration.py score <instrument>       render the model at the same notes and dynamics, and compare
    tools/iowa_calibration.py fit <brass>              fit a brass bell's high-pass and tone low-pass to the recordings

<instrument> is horn, trumpet, trombone, tuba, flute, oboe, clarinet, violin, viola, cello or bass. The recordings are analysis inputs only:
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


def fetch():
    record = json.loads(PROVENANCE.read_text())
    for source in record["sources"]:
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


if __name__ == "__main__":
    command, *args = sys.argv[1:] or ["help"]
    if command == "fetch" and not args:
        fetch()
    elif command in ("reference", "score") and len(args) == 1 and args[0] in INSTRUMENTS or command == "fit" and args and args[0] in BRASS:
        {"reference": reference, "score": score, "fit": fit}[command](args[0])
    else:
        sys.exit(__doc__)
