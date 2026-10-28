# PianoCraft

PianoCraft is an original, clean-room CLAP piano instrument for macOS. It uses
nonlinear hammer excitation, per-note modal strings, effective unison modes,
damper and sustain behavior, sympathetic resonance, and regional soundboard
filters. It does not play reference samples or use Pianoteq data.

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
The first version uses Bitwig's generic parameter controls; no custom editor is
required.

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
