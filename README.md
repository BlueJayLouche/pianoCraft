# PianoCraft

PianoCraft is an original, clean-room CLAP piano instrument for macOS. It uses
a hysteretic felt hammer (Stulov's published hammer set) striking physically
scaled modal strings, coupled unison strings (Weinreich), phantom partials from
longitudinal string motion (Bank & Sujbert), damper and sustain behavior,
sympathetic resonance, and regional soundboard filters. It does not play
reference samples or use Pianoteq data.

## Build

Requirements: macOS, Apple Clang, CMake 3.28+, Ninja, and network access for the
pinned CLAP 1.2.10 SDK.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Render and benchmark

```sh
./build/pianocraft-render renders/c4.wav 60 0.8 8
./build/pianocraft-benchmark
```

The renderer arguments are output path, MIDI note, velocity, seconds, sustain
pedal, body level, sympathy level, and mic position (0 spaced pair, 1 coincident,
2 player). Optional numeric arguments use the engine
defaults when omitted. Raw calibration recordings and renders are not included
in the plugin.

## Validate CLAP

Install the pinned 0.4.1 validator locally, then validate and fuzz the actual
bundle. The release archive is verified against its published SHA-256 digest.

```sh
python3 tools/fetch_validator.py
build/tools/clap-validator-0.4.1/clap-validator validate build/PianoCraft.clap --only-failed
build/tools/clap-validator-0.4.1/clap-validator fuzz -j2 -d30s build/PianoCraft.clap
```

## Install for Bitwig

Check that `~/Library/Audio/Plug-Ins/CLAP/PianoCraft.clap` is not an unrelated
existing bundle, then run:

```sh
cmake --install build --config Release
```

Rescan plug-ins in Bitwig Studio and insert PianoCraft on an instrument track.
There is no custom editor. Bitwig shows the parameters in four groups (Hammer,
Strings, Tuning, Mix) and maps two remote-control pages onto its device knobs:

- **Main:** Hammer Hardness, Dynamic Range (pp-ff loudness spread), Velocity
  Brightness, Sustain Length, Sympathetic Level, Body Level, Stereo Width, Output Gain.
- **Voicing:** Unison Detune (how far each unison's strings are apart: more
  beating and more aftersound, as on a real piano; 0 is perfectly in tune and
  leaves a single decay), Aftersound (level of the long second-stage tail),
  Damper Release, Hammer Noise, Mic Position,
  Master Tune (A4 in Hz), Stretch (1 is the recorded piano's octave stretch,
  0 equal temperament, 2 double).

The recorded piano sat at A4 = 439.02 Hz; Master Tune puts A4 exactly where it
says, so the default plays 3.9 cents above the recordings.

The sustain pedal (CC64) is continuous: dampers start lifting at a quarter of
the travel and are fully off at three quarters, so a continuous pedal can
half-pedal, and pressing it again catches a note that is dying away. On/off
pedals behave as before. CC67 is the soft pedal: like an upright's, it shortens
the hammer throw, so notes come out softer and darker.

Per note: Hammer Hardness and Hammer Noise accept Bitwig's polyphonic
modulation (each note keeps its own modulator), and the piano roll's Volume
and Pan note expressions apply per note. A repeated key re-strikes its own
string. The plugin tells Bitwig when each voice actually stops, so a
pedalled note keeps its modulators until it has died away.

At 1.0 every voicing control is the calibrated piano. Sustain Length, Damper
Release, Unison Detune, Aftersound and the tuning controls recompute the string
model (and the sympathetic strings with it) when changed,
so they suit setting by hand more than fast automation.

## Calibration and provenance

See `docs/clean-room.md`, `model/provenance.json`, and
`model/calibration-report.json`. The CC0 Meta Piano recordings are used only to
derive synthesis coefficients. The `calibration/` directory and raw audio are
not distributed with the plugin.

To score the current build against the corpus and check the fitted model laws
(see `docs/model-laws.md`):

```sh
.venv/bin/python tools/evaluate_model.py --refit
```

`tools/calibrate.py` reproduced the original measured tables but is now frozen:
its layer-2 gate closed when the final validation was recorded. Its original
instructions were:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r tools/requirements.txt
.venv/bin/python tools/calibrate.py
```
