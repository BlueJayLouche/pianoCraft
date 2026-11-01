# Oboe and clarinet

The first of the reed family, for *Faune*'s other solo voices: PianoCraft Oboe and PianoCraft
Clarinet, plugins of their own in the bundle. Both are played by the same monophonic wind player
as the flute (`MonoWind`): overlapping or touching notes slur at the keys' speed, the old note giving
way to the new without sliding through the pitches between (`docs/harp-and-flute.md`), a repeated pitch
is tongued, a held note holds, CC1 or the note's Pressure is the breath, velocity shapes only the
attack, CC21 scales the vibrato, CC11 trims, pitch bend is ±2 semitones.

No reference corpus yet: every law below is analytic, with constants set by ear and by
`tests/orchestra_tests.cpp`. The clean-room policy applies unchanged.

## The sound

| Law | Status | Source and check |
|---|---|---|
| Reed: a pressure-controlled valve, reflection min(1, max(-1, 0.7 - 0.3 (p_in - p_mouth))): the closed reed reflects the bore's wave, the open one lets the mouth's pressure in | Analytic form | J. O. Smith's reed table (ICMC 1986; *Physical Audio Signal Processing*). |
| Clarinet bore: a cylinder closed at the reed and open at the bell, the bell inverting the wave, so the loop is half the period and only odd harmonics resonate | Analytic | D3 at forte: the 2nd harmonic 46 dB under the 3rd. |
| Oboe bore: a cone, taken as a cylinder open at both ends with the reed 0.3 of the way along (to 0.35 up the range), where the cone's missing tip would be; the reed hears the bell's wave and that wave's inverted reflection from the tip | Analytic form, position by ear | Scavone's conical-bore form (CCRMA thesis, 1997; STK's Saxofony). With a symmetric two-port junction instead, or the reed nearer the tip, the low notes overblew to the octave or never settled; a one-sided bore reflecting without inversion did not settle at all. B-flat 3 at forte: the 2nd harmonic level with the 1st. |
| The bell's losses: a one-pole low-pass in the reflection, pole 0.5 at 48 kHz, darker at low Brightness | Chosen by ear | |
| The mouth's pressure: a steady regime above the reed's threshold, 0.70 of what shuts it at the lowest note to 0.78 (clarinet) or 0.80 (oboe) at the highest | Measured on the model | The reed table is all or nothing: each note is silent until about 0.55-0.6, then near full level, and above that changes only ~3 dB. So CC1 doesn't drive this pressure. |
| CC1, the dynamic: how far the player lets the reed open sets the level (breath^0.9), and at soft dynamics the upper partials fall away: a low-pass at f0 x (1.2 + 6 b^2) (clarinet) or f0 x (2 + 8 b^2) (oboe) | Analytic laws, constants by ear | Dalmont and Kergomard's elementary clarinet model (amplitude with the reed's opening); Benade on spectral enrichment. CC1 0.2 to 1: oboe +14 dB and 13 dB richer, clarinet +15 dB and 21 dB richer; nearly pure at pp. |
| Radiation: the tone holes pass the bore's pressure rising 6 dB an octave up to their cutoff (clarinet 700 Hz, oboe 1 kHz); the oboe adds its nasal resonances at 1.1 and 2.8 kHz | Chosen by ear | |
| Tuning: the oboe's loop runs flat at the top, 4 cents x (f / kHz)^2 taken back | Fitted | Both within 2.5 cents across their ranges at 48 and 96 kHz, pp and ff. |
| Breath noise: turbulence in the mouth's pressure, more for the tongue's first 20 ms and while a slur's new resonance takes over | Chosen by ear | Breath Noise scales it. |

The clarinet sounds D3 to B-flat 6 (the B-flat clarinet's written E3 to C7), straight-toned by
default; the oboe B-flat 3 to A6 with a gentle vibrato (Vibrato Depth 0.6).

## Next

- Cor anglais and bassoon: the oboe's cone, longer, with their own formants.
- A calibration corpus, as for the other voices: spectra by note and dynamic first.
