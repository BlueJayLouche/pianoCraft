# The pianos

PianoCraft's piano engine plays any piano described as data. Each piano is its measured and fitted tables,
generated into `src/model/pianos/<name>/`, and the same engine, laws and controls play every one of them:

| Piano | Plugin | Folder | Corpus | Licence |
|---|---|---|---|---|
| Upright | PianoCraft Upright | `pianos/meta` | Meta Piano (Meta Open Sound): 4 velocity layers, all 88 keys | CC0 (`model/provenance.json`) |
| Grand | PianoCraft Grand | `pianos/steinway` | University of Iowa MIS: Steinway & Sons model B, pp / mf / ff | "any projects, without restrictions" (`model/iowa-provenance.json`) |

The recordings are analysis inputs only. They stay in `calibration/` (git-ignored), and nothing of them
reaches the plugin but fitted constants (`docs/clean-room.md`).

## How a piano is built in

- `src/model/PianoData.hpp`: the layout every piano's data shares (the per-key calibration, the 1/12-octave
  bands and the 112 partials of the fitted tables), and `PianoData`, which gathers one piano's tables.
- `src/model/pianos/<name>/`: three generated headers in the namespace `pianocraft::pianos::<name>`:
  `CalibrationData.hpp` (each key's fundamental, stiffness, partials, stereo balance and velocity law),
  `PartialTuning.hpp` (each partial's measured frequency) and `SpectrumCalibration.hpp` (strike positions,
  radiation, each key's own partials, velocity and brightening, and the two-stage decays).
- `src/model/Pianos.cpp`: the list of built-in pianos, in the engine's Piano parameter's order. Only this file
  includes the pianos' data.
- `InstrumentModel` builds from one `PianoData`. The engine builds a model, and its packed mode banks, for every
  built-in piano at `prepare`. New notes play the selected piano and each note keeps the piano it was struck
  on, so switching never steps a ringing string. Voicing and the mic position reach the selected piano, and
  whichever is selected next.
- `src/host/ClapHost.cpp`: each piano is its own plugin in the bundle (an engine with its Piano parameter set,
  the Upright's parameters and pages, and its own presets), so a project's instrument names what it plays.

## Adding a piano

1. **Describe its corpus** in `tools/pianos.py`: a `Piano` with its folder name, its index (its place in
   `Pianos.cpp`), its velocity layers (1, 3 and 4 are the fits' velocity anchors 40, 80 and 120 of 127; 2 is
   60) and a path for each key and layer, `None` where the corpus has none. Correct any wrong labels there,
   by pitch, and record them in the corpus's provenance. Mark it `room=True` if its files carry a room's noise near
   a soft note's level (see the Grand below).
2. **Fetch it** with its checksums in a provenance record (for Iowa's, `tools/iowa_calibration.py fetch piano`).
3. **Measure it**: `PIANOCRAFT_PIANO=<name> .venv/bin/python tools/fit_piano.py measure` writes its
   `CalibrationData.hpp`. Then `neutral` writes flat spectrum tables, and a zero tuning table starts it.
4. **List it** in `src/model/Pianos.cpp` and build.
5. **Fit it**: `tune`, rebuild, `strike`, `decays`, rebuild, `spectrum 6`, all with `PIANOCRAFT_PIANO=<name>`.
   The tools render it through the engine's Piano parameter (`18=<index>`).
6. **Score it**: `PIANOCRAFT_PIANO=<name> .venv/bin/python tools/evaluate_model.py` and `fit_piano.py score`.
7. **Give it a plugin** in `ClapHost.cpp`, with presets (marker files in `resources/presets/`).

`fit_piano.py measure` replaces, for a new piano, the measurement that produced the Upright's calibration
before this repository began (`tools/calibrate.py` only refits that frozen data). From C4 up the fundamental
dominates: it is the strongest peak within 60 cents of the key, and the partials above it are searched tightly
about the stiff-string law. Below C4 a wound string's fundamental is weak, so the series is followed from where
the law puts it, and the fundamental is the series' fit. The stiffness takes the median of the reliable keys
nearby (6 partials or more), and above the last of them continues their log-linear rise from C4: the top two
octaves' few partials leave a fit loose. The stereo balance is evened over 5 keys like the tables.

## The Grand: Iowa's Steinway B

Recorded by Evan Mazunik on 5 and 27 November 2001 in the Voxman Music Building, with two Neumann KM 84s
8 inches above the bass and the treble strings (16-bit, 44.1 kHz, stereo, a room, not anechoic). Each note is
held to its decay, 7-55 s. The dynamics keep their levels: pp to mf to ff rises about 13-14 dB at C4.

Several of Iowa's labels are wrong, found by pitch (a harmonic sieve in the bass, the fundamental's peak against
the other dynamics in the treble):

- ff's "A0" holds A#0, and its "A#0" and "B0" hold more takes of C1;
- pp's "C1" holds B0;
- mf's "G7"-"C8" each hold the note a semitone below (F#7-B7): mf has no C8, where its labels have no Gb7.

`tools/pianos.py` maps them, leaving ff without A0 and B0, pp without C1 and mf without C8.

Every file carries the hall's low rumble at about -67 dBFS, which reaches a pp note's onset threshold (5% of its
peak): C4 pp's onset was found 0.4 s before its attack, and every soft note's measurements landed in the rumble
(its partials stood 41-49 dB over their floor where mf's stand 90, and the first fit missed pp by 12 dB). Its
`Piano` is marked `room`, so the tools' onset also stands 20 dB over each file's own floor. Meta's keeps the plain
threshold its fits were made with. The other files
sit within 6 cents (median) of their keys' stretch. The piano is stretched from about -25 cents at A0 to
+45-60 at the top, with A4 at 441 Hz.

Its laws are the Upright's: the engine's string scale, hammers, unisons, dampers and board are the same, and
the fitted tables carry what differs. The velocity laws are the Upright's register values, which the fitted
velocity tables refine.

### How close it is

Scored as the Upright is (`tools/evaluate_model.py` and `fit_piano.py score`, fitting keys; the Upright's in brackets):

| Metric | Grand | Upright |
|---|---|---|
| Partial level error pp / mf / ff | 5.7 / 4.5 / 7.2 dB | 5.0 / 4.1 / 5.0 |
| Spectrum error, fitting layers | 5.36 dB | 4.82 |
| Key level std, mf | 2.37 dB (C#5-D#5 6-8 dB loud: between the microphones, the recordings dip there) | 1.94 |
| Sustain error, mf | 10.6 dB | 9.5 |

What remains:

- **The bass's top at ff.** A0-B2's partials above 4 kHz stand 5-7 dB weaker than the recordings at ff and mf
  (11 at pp), where microphones 8 inches over a grand's strings hear a lot of them. The tables aren't clipped (4%
  at +40 dB); twelve rounds of the spectrum fit leave ff at 7.2 dB. The model's hammer gives less there.
- **pp loudness in the treble.** From C5 up a pp note's first half-second is 11 dB louder against mf in the
  recordings than in the model, though its partials are fitted absolutely: the rest is the hammer and action
  the close microphones hear. The velocity laws stay the Upright's (lowering them would make the strings too loud).
  The Grand's velocity span reads 5.9 dB wide for it.
- **Between the partials.** The recordings are cleaner between partials than the model, whose board fills them:
  the phantom check reads +13.5 dB, and +7.8 with the phantoms off entirely, so the Grand keeps the Upright's
  phantom coupling (0.03) until a measure separates them.

Each voice is scaled by the Grand's output trim (0.29, `Pianos.cpp`), so its ff chords peak as the Upright's do;
its fitted absolute level is the Steinway recordings', 8 dB louder. `tools/render.cpp` divides it out.
