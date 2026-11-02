# Strings and horn

The last two voices of the final act (`docs/old-house-needs.md`, section 6): PianoCraft Strings and
PianoCraft Horn, plugins of their own in the PianoCraft bundle. They follow the same conventions
as the harp and flute: CC1 is the dynamic (bow or breath), velocity shapes only the attack, CC11
trims the output, CC64 holds released notes, pitch bend is ±2 semitones, and articulations are
parameters and presets, not keyswitches. A note's own Pressure is its dynamic too, overriding CC1
for that note: `piano.py` writes each CC1 curve again as Pressure, the expression Bitwig imports
from a `.dawproject`.

The strings' bodies, rosin and bow arm are calibrated against recordings (below); the other laws are
analytic: published physics, constants set by ear and by `tests/orchestra_tests.cpp`. The horn is
calibrated too (`docs/brass.md`). The clean-room policy applies unchanged.

## Strings

Each note is played by Section Size players (1 is a soloist, 6 by default), each a bowed-string
waveguide: two delay lines, bow to bridge and bow to finger, joined at the bow by its friction.
Notes go to a body by register: basses below C2, cellos below C3, violas below G3, violins above.
The score's strings are con sordino, so Con Sordino is on by default.

### Calibration

Fitted to the University of Iowa Musical Instrument Samples' violin, viola, cello and double bass:
anechoic, mono, one bowed note at a time on each string, at pp, mf and ff, without vibrato. Sources,
licence and checksums are in `model/iowa-provenance.json`; the recordings stay in `calibration/`
(git-ignored), analysis inputs only. `tools/iowa_calibration.py` compares them as it does the brass
(`docs/brass.md`), with the model played solo, open, without vibrato, at CC1 0.25, 0.6 and 1.0. The
string runs skip notes, so each recorded note is named by its own pitch (YIN).

Each body was fitted on every recording of its instrument (200-270 notes each), not just the register
the model gives it: the bowed string is the model's render at that note, its old body divided out.
Spectral error, mean dB per note over the family's own register:

| | Before, pp / mf / ff | Now, pp / mf / ff | Other |
|---|---|---|---|
| Violin (G3 up) | 8.6 / 7.3 / 7.2 | 6.7 / 6.3 / 6.4 | Noise between harmonics 4-8 dB too clean, now within 3 dB |
| Viola (C3-F#3) | 13.3 / 11.9 / 14.7 | 7.8 / 8.9 / 11.2 | It was 10-13 dB too bright above 3 kHz, now 2-5 |
| Cello (C2-B2) | 15.3 / 12.7 / 14.9 | 10.8 / 10.1 / 11.1 | It was 18-24 dB too bright above 3 kHz, now 2.5 at pp and mf, 12 at ff |
| Bass (E1-B1) | 20.0 / 21.1 / 21.8 | 7.0 / 6.6 / 6.7 | It was 20-30 dB too bright above 3 kHz; 6-13 dB too noisy, now 4-9 |

Three recordings of one cello note differ by about as much as the model does from them, so the
cello's 10-11 dB is near what one player's notes allow. Attacks are left to velocity: the
recordings' pp entries swell in up to 0.6 s, a player's choice rather than the instrument's. The
players' level also wanders far more than the model's (1-6 dB against 0.4-0.9), much of it slow
swells within each long bow.

| Law | Status | Source and check |
|---|---|---|
| Bow friction: the string sticks to the bow until their velocities differ enough, then slips; reflection min(1, (\|dv\| (5 - 4 force) + 0.75)^-4) | Analytic form | J. O. Smith's bow table (ICMC 1986; *Physical Audio Signal Processing*). Helmholtz motion at every key E1-E7. |
| CC1 is the bow: speed 0.03 + 0.17 b and force 0.25 + 0.75 b together, the bow point from 0.13 of the string (pp) to 0.10 (ff); a harder bow keeps more upper partials, a shelf above 1.5 kHz of 1.2 (b - 0.5) | Analytic, the shelf chosen by ear | Players play louder nearer the bridge (Schelleng, JASA 53, 1973; Askenfelt, JASA 86, 1989); force sharpens the Helmholtz corner the losses round off (Cremer, *The Physics of the Violin*). In the waveguide that sharpening barely shows (the bridge loss changes the spectrum under 1 dB), so the shelf carries it. A4 0.3 to 1.0: +10.7 dB, and the level above 3 kHz +4.8 dB. Bowing nearer the bridge without more force fell into double slip (H2 over H1). |
| Brightness (and per-note Timbre) moves the bow's shelf: (Brightness - 1) added to it, from -12 dB at 0.25 to +6 dB at 2 above 1.5 kHz | Chosen by ear | It also sets the bridge loss and bow force, but those change a steady bowed tone by under a dB; alone, the knob did nothing audible. A4 0.25 to 2: 12 dB brighter. |
| Rosin and the bow arm: the friction's force jitters (±18%, noise below 2 kHz; ±6% on the bass), so no two slips are alike; the bow's speed wanders ±20% and the vibrato's depth ±30% and rate ±8%, a new way every 0.15-0.45 s | Fitted (the rosin and the bow's wander) | Against the recordings' noise between harmonics and the steadiness of their level: more rosin on the bass unsettled its stick-slip, less spoiled its spectrum. Real bowing is aperiodic (McIntyre, Schumacher and Woodhouse, "Aperiodicity in bowed-string motion", Acustica 1981). Without it the bowing sounded mechanical. Seeded per note and player, so renders repeat. |
| Tuning: the loop less the bridge filter's phase delay, then 2 cents per kHz taken back | Fitted | Within 2.5 cents C2-E7 at 48 and 96 kHz, read at the 2nd harmonic: the cello's low C, like the recordings', radiates its fundamental 17-30 dB under its strongest harmonic. |
| Body: ten modes, at the violin's A0 275 Hz, CBR, B1-, B1+ and higher, and its bridge hill near 2.5 kHz, scaled by size (the hill by its square root): 0.88 violin, 0.83 viola, 0.41 cello, 0.29 bass. Each family has its own signed mode gains, a share passed direct, and a two-pole roll-off (5.4 kHz violin, 950 Hz viola, 1.4 kHz cello, none on the bass) | Fitted (the modes' frequencies and Q analytic) | Bissinger's signature modes; Jansson's bridge hill. The scaled violin body left the larger instruments far too bright and their fundamentals too strong: a viola's or cello's low notes sit below its air mode and radiate their fundamentals weakly, and the bass is dark above 500 Hz. Each family keeps its old loudness at mf. |
| Con sordino: a 1.2 kHz low-pass into the body and its modes above 1 kHz at a quarter | Chosen by ear | A mute's mass on the bridge (Cremer). A4: 6 dB less above 3 kHz. |
| Sul tasto: the bow point 0.15 further from the bridge, 40% less force | Analytic | Darker by 5-11 dB above 2 kHz from G3 up. |
| Harmonics: the half string bowed lightly at 0.3, the touching finger damping the upper modes (a heavy bridge loss), 8.5 cents per kHz taken back | Chosen by ear | Purer: 15-20 dB less above 2 kHz than a muted note. In tune within 5 cents. |
| Tremolo: the bow reverses about 14 times a second, each player at their own pace, in sharp turns (tanh(6 sin)), strokes 1.4x a sustained bow's speed, the force lightening to 30% as the bow turns | Chosen by ear | At first the bow lingered near zero speed at full force, gripped the string and rebuilt it from nothing every stroke: 9 dB quieter than a held note and scrappy. Now the string rings through: a 3 dB pulse, about 1 dB under a held note. A section's reversals fall out of step and shimmer, as an unmeasured tremolo does. |
| Section: each player detuned up to ±6 cents x Section Spread, landing up to 30 ms late, bowing ±7% faster, at their own bow point (±15%), vibrato rate and phase; intonation wanders ±2 cents every ~0.25 s | Chosen by ear | Seeded by key and player: the same every render. The players beat, as real ones do, most when muted (the fundamental dominates); held notes don't fade over seconds. |
| Vibrato: ±15 cents x Vibrato Depth, from 0.2 s into a note over 0.5 s; CC21 scales it (64 is the setting) | Chosen by ear | |
| Seats: violins left, violas centre, cellos and basses right, amplitude panning only | Analytic | Stereo Width 0 is bit-identical left and right. |

The release is the bow stopping on the string, which damps it: a note ends in about half a second.
Eight notes of eight players each cost 5% of real time at 48 kHz.

## Horn

Each note is a lip-reed waveguide (Cook's TBone, ICMC 1991): the lips are a resonance driven by the
pressure across them, their opening lets the breath into a bore of one period, and the bell
reflects. Each note is its own player, a cent or two from the others. The horn shares its engine
with the trumpet, trombone and tuba, and since November 2026 all four are calibrated against
recordings: its laws, sources and checks are in `docs/brass.md`.

## Next

- By ear: the mute, the section's shimmer, and the cello and bass at ff (still brighter than the
  recordings above 3 kHz).
- The bass's noise between harmonics, 4-9 dB over the recordings, which rosin does not reach.
- A second corpus, so the bodies are not one set of instruments in one room.
