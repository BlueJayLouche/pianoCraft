# Strings and horn

The last two voices of the final act (`docs/old-house-needs.md`, section 6): PianoCraft Strings and
PianoCraft Horn, plugins of their own in the PianoCraft bundle. They follow the same conventions
as the harp and flute: CC1 is the dynamic (bow or breath), velocity shapes only the attack, CC11
trims the output, CC64 holds released notes, pitch bend is ±2 semitones, and articulations are
parameters and presets, not keyswitches. A note's own Pressure is its dynamic too, overriding CC1
for that note: `piano.py` writes each CC1 curve again as Pressure, the expression Bitwig imports
from a `.dawproject`.

Neither has a reference corpus, so every law below is analytic: published physics, constants set
by ear and by `tests/orchestra_tests.cpp`. The clean-room policy applies unchanged.

## Strings

Each note is played by Section Size players (1 is a soloist, 6 by default), each a bowed-string
waveguide: two delay lines, bow to bridge and bow to finger, joined at the bow by its friction.
Notes go to a body by register: basses below C2, cellos below C3, violas below G3, violins above.
The score's strings are con sordino, so Con Sordino is on by default.

| Law | Status | Source and check |
|---|---|---|
| Bow friction: the string sticks to the bow until their velocities differ enough, then slips; reflection min(1, (\|dv\| (5 - 4 force) + 0.75)^-4) | Analytic form | J. O. Smith's bow table (ICMC 1986; *Physical Audio Signal Processing*). Helmholtz motion at every key E1-E7. |
| CC1 is the bow: speed 0.03 + 0.17 b and force 0.25 + 0.75 b together, the bow point from 0.13 of the string (pp) to 0.10 (ff); a harder bow keeps more upper partials, a shelf above 1.5 kHz of 1.2 (b - 0.5) | Analytic, the shelf chosen by ear | Players play louder nearer the bridge (Schelleng, JASA 53, 1973; Askenfelt, JASA 86, 1989); force sharpens the Helmholtz corner the losses round off (Cremer, *The Physics of the Violin*). In the waveguide that sharpening barely shows (the bridge loss changes the spectrum under 1 dB), so the shelf carries it. A4 0.3 to 1.0: +10.7 dB, and the level above 3 kHz +4.8 dB. Bowing nearer the bridge without more force fell into double slip (H2 over H1). |
| Brightness (and per-note Timbre) moves the bow's shelf: (Brightness - 1) added to it, from -12 dB at 0.25 to +6 dB at 2 above 1.5 kHz | Chosen by ear | It also sets the bridge loss and bow force, but those change a steady bowed tone by under a dB; alone, the knob did nothing audible. A4 0.25 to 2: 12 dB brighter. |
| Rosin and the bow arm: the friction's force jitters (±6%, noise below 2 kHz), so no two slips are alike; the bow's speed wanders ±5% and the vibrato's depth ±30% and rate ±8%, a new way every 0.15-0.45 s | Chosen by ear | Real bowing is aperiodic (McIntyre, Schumacher and Woodhouse, "Aperiodicity in bowed-string motion", Acustica 1981). Without it the bowing sounded mechanical. Seeded per note and player, so renders repeat. |
| Tuning: the loop less the bridge filter's phase delay, then 2 cents per kHz taken back | Fitted | Within 2.5 cents C2-E7 at 48 and 96 kHz. |
| Body: violin modes A0 275 Hz, CBR, B1-, B1+ and higher, and a bridge hill near 2.5 kHz; viola, cello and bass scale the frequencies by 0.82, 0.42, 0.26 (the hill by their square roots) | Analytic, set by ear | Bissinger's signature modes; Jansson's bridge hill. |
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
reflects. Each note is its own player, a cent or two from the others.

| Law | Status | Source and check |
|---|---|---|
| Lips: a resonant low-pass (Q 8) at 0.8 of the note, so they sit open under the breath; opening max(0, swing)^2, clipped at 1 | Analytic form, constants by ear | Brass lips are an outward-striking valve and sound above their resonance (Fletcher & Rossing, *The Physics of Musical Instruments*). A band-pass lip (no static opening) never started; the lips at the note pulled the pitch 50-90 cents with breath. Lips that close rather than open backwards give the even partials (H2 5-12 dB below H1). |
| Tuning: the loop less the bell's and DC blocker's phase delay, then 17 cents taken back | Fitted | Within 6 cents B-flat 1-F5 at 48 and 96 kHz, rising a few cents with breath as brass does. |
| CC1 is breath: mouth pressure 0.35 + 0.65 b | Analytic | Spectral enrichment with breath, as in brass (Benade): C4 0.3 to 1.0 is +7 dB, and +12 dB more above 3 kHz. |
| Bell: reflects through a 2.5 kHz low-pass, radiates 6 dB/octave less below 500 Hz | Chosen by ear | Low notes' fundamentals are weak, as on a horn. |
| Hand in the bell: two one-pole low-passes at 1.5 kHz x Brightness x (0.5 + 0.8 b) | Chosen by ear | The horn's dark tone; Distant Horn sets Brightness 0.6. |
| Stopped: the hand closes the bell: what escapes is a 2.8 kHz resonance (Q 3) of the bell's sound, blown 20% harder | Chosen by ear | Quieter (about 7 dB at C4) and thin, nasal and metallic. The written pitch sounds: a player fingers a semitone down. |
| Attack: the breath arrives in 60 ms (pp) to 15 ms (ff), with a tongued puff up to +40% that settles within 60 ms | Chosen by ear | Speaks within 15 ms (ff) to 30 ms (pp), stopped up to about 25 ms. |

## Next

- A calibration corpus, as for the harp and flute.
- By ear first: the strings' body and mute, the section's shimmer, and the horn's hand and stopped tone.
