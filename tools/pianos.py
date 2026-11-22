"""The pianos the tools measure, fit and score: each one's recordings, velocity layers and generated data.

PIANOCRAFT_PIANO picks one (default meta). Its index is the engine's Piano parameter (ParameterId::piano), and its
generated headers live in src/model/pianos/<name>/. The layers are the fits' velocity anchors, of 127: 1 is 40,
2 is 60, 3 is 80 (the reference layer) and 4 is 120; a corpus without a layer leaves it out.
"""
from __future__ import annotations

import json
import os
from functools import lru_cache
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHARPS = ("C", "Cs", "D", "Ds", "E", "F", "Fs", "G", "Gs", "A", "As", "B")
FLATS = ("C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B")
SPLIT = set(json.loads((ROOT / "model" / "provenance.json").read_text())["calibration"]["held_out_midi_notes"])


def octave_name(midi: int, names=SHARPS) -> str:
    return f"{names[midi % 12]}{midi // 12 - 1}"


class Piano:
    def __init__(self, name, index, title, layers, path, cache=None, room=False):
        self.name, self.index, self.title, self.layers, self._path = name, index, title, layers, path
        # The recordings carry a room's noise near a soft note's onset threshold (evaluate_model.onset).
        self.room = room
        self.folder = ROOT / "src" / "model" / "pianos" / name
        self.calibration_header = self.folder / "CalibrationData.hpp"
        self.spectrum_header = self.folder / "SpectrumCalibration.hpp"
        self.tuning_header = self.folder / "PartialTuning.hpp"
        self.cache = cache or ROOT / "calibration" / f"{name}-spectrum-reference.npz"

    def path(self, midi: int, layer: int) -> Path:
        return self._path(midi, layer)

    @lru_cache(maxsize=None)
    def has(self, midi: int, layer: int) -> bool:
        if layer not in self.layers:
            return False
        path = self.path(midi, layer)
        return path is not None and path.exists()

    def keys(self, layer: int = 3) -> list[int]:
        """The keys recorded at a layer."""
        return [m for m in range(21, 109) if self.has(m, layer)]


META = Piano(
    "meta", 0, "Upright", {1: 40 / 127, 2: 60 / 127, 3: 80 / 127, 4: 120 / 127},
    lambda m, l: ROOT / "calibration" / "meta-piano" / f"INSPiano_Vel0{l}_{octave_name(m)}_01.wav",
    cache=ROOT / "calibration" / "spectrum-reference.npz")
# University of Iowa MIS, Steinway & Sons model B (model/iowa-provenance.json): pp, mf and ff as layers 1, 3 and 4,
# fetched, checked and converted to WAV by tools/iowa_calibration.py fetch.
# Iowa's labels, corrected by pitch (a harmonic sieve in the bass, the fundamental's peak against the other dynamics in
# the treble): ff's "A0" holds A#0, and its "A#0" and "B0" more takes of C1; pp's "C1" holds B0; mf's "G7"-"C8" hold
# F#7-B7, so mf has no C8 where the labels have no Gb7. The rest sit within 6 cents (median) of their labels' stretch.
IOWA_DYNAMICS = {1: "pp", 3: "mf", 4: "ff"}
IOWA_LABELS = {(4, 21): None, (4, 22): 21, (4, 23): None, (1, 24): None}


def iowa_piano(midi: int, layer: int):
    label = IOWA_LABELS.get((layer, midi), midi + 1 if layer == 3 and midi >= 102 else midi)
    if label is None or label > 108:
        return None
    return ROOT / "calibration" / "iowa-mis" / "piano" / "wav" / f"Piano.{IOWA_DYNAMICS[layer]}.{octave_name(label, FLATS)}.wav"


STEINWAY = Piano("steinway", 1, "Grand", {1: 40 / 127, 3: 80 / 127, 4: 120 / 127}, iowa_piano, room=True)

PIANOS = {piano.name: piano for piano in (META, STEINWAY)}
PIANO = PIANOS[os.environ.get("PIANOCRAFT_PIANO", "meta")]
