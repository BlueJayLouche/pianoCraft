# PianoCraft

PianoCraft is an original, clean-room CLAP piano instrument for macOS, Windows and Linux. It uses
a hysteretic felt hammer (Stulov's published hammer set) striking physically
scaled modal strings, coupled unison strings (Weinreich), phantom partials from
longitudinal string motion (Bank & Sujbert), damper and sustain behavior,
sympathetic resonance, and regional soundboard filters. Each key's strike point,
each partial's level, how it brightens with velocity and its two-stage decay are
fitted to the CC0 Meta Piano recordings (`tools/fit_piano.py`). It does not play
reference samples or use Pianoteq data.

## Build

Requirements: CMake 3.28+, a C++17 compiler (Apple Clang, MSVC or GCC), and
network access for the pinned CLAP 1.2.10 SDK. The commands below are for macOS
with Ninja; on Windows, drop `-G Ninja` and pass `--config Release` to the build
and `-C Release` to ctest. Releases are built by CI for all three platforms.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Render and benchmark

```sh
./build/pianocraft-render renders/c4.wav 60 0.8 8
./build/pianocraft-render-voice harp renders/harp-c4.wav 60 0.8 8
./build/pianocraft-render-voice flute renders/flute-a4.wav 69 0.7 4 3.5 cc1=0.6
./build/pianocraft-render-voice strings renders/strings-a4.wav 69 0.7 4 3.5 cc1=0.6
./build/pianocraft-render-voice horn renders/horn-c4.wav 60 0.7 4 3.5 cc1=0.6
./build/pianocraft-render-voice trumpet renders/trumpet-g4.wav 67 0.7 4 3.5 cc1=0.6
./build/pianocraft-render-voice oboe renders/oboe-a4.wav 69 0.7 4 3.5 cc1=0.6
./build/pianocraft-render-voice celesta renders/celesta-c5.wav 72 0.7 4 1
./build/pianocraft-render-voice crotales renders/crotale-e6.wav 88 0.7 8
./build/pianocraft-render-voice vibraphone renders/vibraphone-f4.wav 65 0.7 6 6 7=3.5
./build/pianocraft-render-voice handpan renders/handpan-d4.wav 62 0.7 6
./build/pianocraft-benchmark
```

The renderer arguments are output path, MIDI note, velocity, seconds, sustain
pedal, body level, sympathy level, and mic position (0 spaced pair, 1 coincident,
2 player). Optional numeric arguments use the engine
defaults when omitted. Raw calibration recordings and renders are not included
in the plugin. `pianocraft-render-voice` takes the instrument, output, key, velocity,
seconds, then optionally the release time and `id=value` parameters or `ccN=value`
controllers.

## Validate CLAP

Install the pinned 0.4.1 validator locally, then validate and fuzz the actual
bundle. The release archive is verified against its published SHA-256 digest.

```sh
python3 tools/fetch_validator.py
build/tools/clap-validator-0.4.1/clap-validator validate build/PianoCraft.clap --only-failed
build/tools/clap-validator-0.4.1/clap-validator fuzz -j2 -d30s build/PianoCraft.clap
```

## Install for Bitwig

On Windows and Linux, copy `PianoCraft.clap` and the `PianoCraft` folder beside
it (its presets) from the build folder into `C:\Program Files\Common Files\CLAP` or
`~/.clap`. The editor is macOS-only; there the host shows its own parameter view.

On macOS, check that `~/Library/Audio/Plug-Ins/CLAP/PianoCraft.clap` is not an
unrelated existing bundle, then run:

```sh
cmake --install build --config Release
```

Rescan plug-ins in Bitwig Studio and insert PianoCraft on an instrument track.
Each instrument has a simple editor: one column of its parameters, grouped by
module, with a slider per value and a pop-up per named choice. Bitwig also shows
the parameters in five groups (Hammer,
Strings, Tuning, Age, Mix) and maps three remote-control pages onto its device knobs:

- **Main:** Hammer Hardness, Dynamic Range (pp-ff loudness spread), Velocity
  Brightness, Sustain Length, Sympathetic Level, Body Level, Stereo Width, Output Gain.
- **Voicing:** Age, Unison Detune (how far each unison's strings are apart: more
  beating and more aftersound, as on a real piano; 0 is perfectly in tune and
  leaves a single decay), Aftersound (level of the long second-stage tail),
  Damper Release, Hammer Noise, Mic Position,
  Master Tune (A4 in Hz), Stretch (1 is the recorded piano's octave stretch,
  0 equal temperament, 2 double).
- **Pedal:** Pedal Noise, Damper Release, Sympathetic Level.

Pedal Noise (0 to 2) is the sustain pedal's own sound, from how fast it moves: the
trapwork's knock through the soundboard, and the damper felt brushing the strings
as it lifts off or lands, which leaves the open strings ringing faintly. A stamp
(or square CC64 automation) knocks loudest: about 39 dB under a mf C4 as the
dampers land and 49 dB as they lift, with the strings' shimmer fainter still. A
pedal moved over 300 ms knocks 20 dB quieter. The dampers ride the pedal's
motion, a full stroke taking about 25 ms. Age makes it up to twice as loud. The
levels are chosen, not measured: the reference recordings have no pedal noise.

Age (0 to 1) wears the piano as one control, a little differently on every key
but identically on every render: pitch slackens about 25 cents with uneven
tuning and stretch, unisons drift apart, some dampers leak, hammers harden
unevenly, and key response turns uneven. At 0 it is the calibrated piano.

Stereo Width 0 is a true mono render: one coherent microphone, so summing to
mono can't comb-filter at any Mic Position. Use it for mono bounces; widths
between 0 and 1 crossfade to the microphone pair.

Bitwig's preset browser lists four built-in voicings: Calibrated Upright
(stereo) and, for mono bounces, Whole, Lightly Worn (Age 0.3) and Most Worn
(Age 1). They ship as marker files in the bundle's `Contents/Resources/Presets`,
because Bitwig lists preset files, not presets held inside a plugin; the values
live in `src/host/ClapHost.cpp`. Save your own as Bitwig presets.

## Harp, flute, oboe, clarinet, strings, brass and struck metal

The bundle also holds PianoCraft Harp, Flute, Oboe, Clarinet, Strings, Horn, Celesta and Crotales, for the final
act of *The Old House on the Corner*, and Trumpet, Trombone, Tuba, Vibraphone and Handpan. They follow the orchestral-library conventions: CC11 is an expression trim,
pitch bend ±2 semitones, no keyswitches. On the winds, strings and brass a note's Pressure is its
dynamic too, as CC1 is: `piano.py` writes each CC1 curve again as Pressure, which Bitwig imports
from a `.dawproject`. Their physics and what is still set by ear are in
`docs/harp-and-flute.md`, `docs/oboe-and-clarinet.md`, `docs/strings-and-horn.md` and
`docs/brass.md`, `docs/celesta-and-crotales.md` and `docs/vibraphone-and-handpan.md`. The brass, woodwinds, strings,
crotales and vibraphone are calibrated against the University of Iowa's recordings (`tools/iowa_calibration.py`),
and the celesta's bars against its orchestra bells. The harp's decays are fitted to VCSL's concert harp
(`tools/fit_harp.py`); the handpan has no reference corpus yet.

- **Harp:** one string per key (B0-G#7). Strings ring after note-off, so glissandi keep sounding.
  The strings the pedals sound (set by what you play) ring in sympathy. Controls: Pluck Position,
  Pres de la Table, Harmonics (the key is the sounding pitch), Brightness (Timbre moves it per note),
  Note-Off Damping (CC64 holds it off), Sustain Length, Sympathetic Level, Body Level,
  Stereo Width (0 is exact mono), Master Tune. Velocity is the dynamic. Presets: Concert Harp, and
  Harp, Pres de la Table and Harmonics in mono.
- **Flute:** monophonic, B3-D7. CC1 is breath (loudness and tone); velocity shapes only the attack.
  Overlapping notes slur without a new attack, and a held note holds. CC21 scales the vibrato,
  which enters after the note speaks. Controls: Breath Noise, Brightness (Timbre moves it), Vibrato
  Depth and Rate, Legato Time, Master Tune. Presets: Solo Flute, and Veiled Flute (breathier,
  darker, little vibrato) for the low *Faune* register.
- **Oboe and clarinet:** reed waveguides played as the flute is (slurs, tonguing, CC1 or Pressure as
  breath, CC21 vibrato). The clarinet (D3-B-flat 6) sounds odd harmonics, straight-toned; the oboe
  (B-flat 3-A6) every harmonic, nasal with its formant near 1.3 kHz, with a gentle vibrato. Both are
  nearly pure at pp and full at ff. Controls as the flute's, and Reed: how much the reed buzzes (0 the
  bore alone, sweet and covered; 1 calibrated; 2 nasal and edgy), at the same loudness. Presets: Oboe,
  Oboe Dolce (a softer reed), Clarinet, Clarinet Dolce.
- **Strings:** a section (E1-E7) by register: basses, cellos, violas, violins. CC1 is the bow
  (loudness and tone); velocity shapes only the attack; CC21 scales the vibrato. Con Sordino is on by default. Controls: Con Sordino, Sul Tasto, Tremolo (bow reversals),
  Harmonics, Section Size (1 is a soloist), Section Spread, Brightness (Timbre moves it), Vibrato
  Depth and Rate, Stereo Width (0 is exact mono), Master Tune. Presets: con sordino in stereo and
  mono, sul tasto, tremolo, harmonics, a soloist, and senza sordino.
- **Horn:** a player per note (B-flat 1-F5). CC1 is breath: louder and brassier. Controls: Stopped,
  Brightness (the hand in the bell; Timbre moves it), Stereo Width, Master Tune. Presets: Horns,
  Distant Horn and Stopped Horn (mono).
- **Trumpet, trombone and tuba:** the horn's engine with their own bells, a player per note: the
  trumpet (E3-C6) brightest, the trombone (E2-F5) between, both turning brassy at ff; the tuba
  (D1-F4) darkest. Controls as the horn's, with Straight Mute for Stopped. Presets: Trumpets and
  Muted Trumpet (mono), Trombones and Muted Trombone (mono), Tuba in stereo and mono. Each is
  levelled by its loudest ff note; within an instrument the levels follow the recordings, so low
  trombone notes at mf are far quieter than high ones.
- **Celesta and crotales:** struck metal, a bar or disc per key; velocity is the dynamic. The celesta (C4-C9,
  sounding) has felt hammers and dampers that fall at note-off unless CC64 holds them. The crotales (C6-C8,
  sounding) ring until they die away: the low discs strike bright and settle to their fundamental, and some
  beat. Controls: Brightness (Timbre moves it), Note-Off Damping, Sustain Length, Master Tune, Stereo Width
  (0 is exact mono). Presets: Celesta and Crotales, each in stereo and mono.
- **Vibraphone and handpan:** struck metal too, with the same controls. The vibraphone (C3-F6) has aluminium bars
  tuned 1:4:10 and dampers, as the celesta's. Motor Speed (0 is off) and Motor Depth turn the resonators' discs,
  which swell only the fundamental. The handpan (C3-C6, chromatic, a tone field per key) is tuned 1:2:3, and its
  octave and fifth shimmer. It rings on after note-off. Presets: Vibraphone in stereo and mono, and with a slow
  motor in mono; Handpan in stereo and mono.

Bitwig's voice stacking plays one key as several notes. On the brass and strings each stacked voice
is another player: its own detune (3-8 cents from the first), entry, breath or bowing, and place. The
piano, harp, celesta, crotales, vibraphone and handpan have one string, bar, disc or tone field per key, so a stacked voice re-strikes
it; the flute, oboe and clarinet are one player each (their slurs need it), so Bitwig offers no
stacking. Stacked voices accept per-voice Volume, Pan and Brightness; pitch spread comes from the
players' own detune.

The recorded piano sat at A4 = 439.02 Hz; Master Tune puts A4 exactly where it
says, so the default plays 3.9 cents above the recordings.

The sustain pedal (CC64) is continuous: dampers start lifting at a quarter of
the travel and are fully off at three quarters, so a continuous pedal can
half-pedal, and pressing it again catches a note that is dying away. On/off
pedals behave as before. CC67 is the soft pedal: like an upright's, it shortens
the hammer throw, so notes come out softer and darker.

Per note: Hammer Hardness and Hammer Noise accept Bitwig's polyphonic
modulation (each note keeps its own modulator), and the piano roll's Volume
and Pan note expressions apply per note. Timbre sets each note's hammer
hardness (centre is neutral, the extremes move it by 1); like a real hammer it
shapes the note at the strike, so later Timbre changes don't alter it. A repeated key re-strikes its own
string. The plugin tells Bitwig when each voice actually stops, so a
pedalled note keeps its modulators until it has died away.

At 1.0 every voicing control is the calibrated piano. Sustain Length, Damper
Release, Unison Detune, Aftersound, Age and the tuning controls recompute the string
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

The strike positions, radiation, velocity and decay tables in `src/model/SpectrumCalibration.hpp` are
regenerated from the recordings (about 15 minutes; each spectrum round renders all 352 notes):

```sh
.venv/bin/python tools/fit_piano.py neutral
.venv/bin/python tools/fit_piano.py strike
.venv/bin/python tools/fit_piano.py decays
.venv/bin/python tools/fit_piano.py spectrum 6
.venv/bin/python tools/fit_piano.py score
```

To compare PianoCraft with another piano by ear, `tools/ab_project.py` writes
`renders/ab/ab-test.dawproject`: the same notes, pedalling and soft pedal on two
tracks, a marker per section. Load PianoCraft Upright on one and the other piano on
the other, with no effects, and bounce both. It's for diagnosis only
(`docs/clean-room.md`).

`tools/calibrate.py` reproduced the original measured tables but is now frozen:
its layer-2 gate closed when the final validation was recorded. Its original
instructions were:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r tools/requirements.txt
.venv/bin/python tools/calibrate.py
```
