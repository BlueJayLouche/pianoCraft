# Oboe and clarinet

The first of the reed family, for *Faune*'s other solo voices: PianoCraft Oboe and PianoCraft
Clarinet, plugins of their own in the bundle. Both are played by the same monophonic wind player
as the flute (`MonoWind`): overlapping or touching notes slur as the pads and fingers move, by way of
a brief fingering between, without sliding through the pitches between (`docs/harp-and-flute.md`), a repeated pitch
is tongued, a held note holds, CC1 or the note's Pressure is the breath, velocity shapes only the
attack, CC21 scales the vibrato, CC11 trims, pitch bend is ±2 semitones.

The bores' laws below are analytic, with constants set by ear and by `tests/orchestra_tests.cpp`.
What the bores leave out is fitted to the University of Iowa recordings (see Calibration). The
clean-room policy applies unchanged.

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
| Breath noise: turbulence in the mouth's pressure, 20% of a 3 kHz noise, more for the tongue's first 20 ms and while a slur's new resonance takes over | Fitted (the steady part) | The model was 20-35 dB cleaner between its harmonics than the recordings; the oboe is now within 1 dB at mf and 4 at ff, the clarinet within 4-7. More noise slowed the attacks. Breath Noise scales it. |
| The clarinet's even harmonics: its mouthpiece, tone holes and bell are not a perfect cylinder's, and above the tone-hole lattice's cutoff the bore no longer cancels the even harmonics. The radiated pressure's square, normalised to its level and high-passed at 1.5 kHz, 0.3 of it | Phenomenological, amount fitted | The cutoff after Benade (*Fundamentals of Musical Acoustics*). The model's evens were 40-50 dB under the odds at every frequency; the recordings' are 15-30 dB under them at low frequencies and level with them above about 1.5 kHz. An unfiltered even term made low notes' 2nd harmonic far too strong. Spectral error at mf 11.6 to 5.3 dB. |
| Output stage, fitted: the oboe rises 6 dB an octave below 1.13 kHz to a low-pass at 10.5 kHz x (1 + 1.1 b^3); the clarinet darkens at pp, three one-poles at 428 Hz x (1 + 30 b^3) x (f0 / C3)^0.7 | Fitted | The oboe's fundamentals are 8-18 dB under its formant at 1-1.7 kHz in the recordings. |
| Breath wander: the breath is never quite steady, a 6 Hz wander of about 2.5% (oboe) or 3% (clarinet) | Fitted | The model's level was 3-10x steadier than the recordings'; now matched. Seeded: renders repeat. |

The clarinet sounds D3 to B-flat 6 (the B-flat clarinet's written E3 to C7), straight-toned by
default; the oboe B-flat 3 to A6 with a gentle vibrato (Vibrato Depth 0.6).

## Calibration

Against the University of Iowa Musical Instrument Samples (oboe, Kira Wharton, 1997; B-flat
clarinet, Christine Bellomy, 1998; anechoic, pp, mf and ff): sources and checksums in
`model/iowa-provenance.json`, the analysis in `tools/iowa_calibration.py` (as for the brass,
`docs/brass.md`). Spectral error, mean dB per note, pp / mf / ff:

| | Before | Now |
|---|---|---|
| Oboe | 6.8 / 8.5 / 7.9 | 6.4 / 7.3 / 7.7 |
| Clarinet | 9.7 / 11.6 / 11.5 | 4.3 / 5.4 / 6.1 |

The oboe stays furthest from its recordings: its cone takes the reed 0.3-0.35 of the way along the
bore, which cancels the harmonics near multiples of three, where the real oboe's formant peaks (at
A4 the model's 3rd harmonic is 19 dB down, the recording's is its strongest). Nearer the tip, the
reed overblows to the octave on most notes. Both reeds speak 10-40 ms later than the recordings.

## Next

- The oboe's bore: a reed nearer the cone's tip that still speaks, for its 3rd harmonic.
- Cor anglais and bassoon: the oboe's cone, longer, with their own formants.
