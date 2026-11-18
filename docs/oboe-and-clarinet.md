# Oboe and clarinet

The first of the reed family, for *Faune*'s other solo voices: PianoCraft Oboe and PianoCraft
Clarinet, plugins of their own in the bundle. Both are played by the same monophonic wind player
as the flute (`MonoWind`): overlapping or touching notes slur as the pads and fingers move, by way of
a brief fingering between, without sliding through the pitches between (`docs/harp-and-flute.md`), a repeated pitch
is tongued, a held note holds, CC1 or the note's Pressure is the breath, velocity shapes only the
attack, CC21 scales the vibrato, CC11 trims, pitch bend is ±2 semitones. Reed (0 to 2, 1 calibrated) is how
much the reed buzzes: at 0 the bore alone, sweet and covered; above 1 nasal and edgy. It moves the tone, not
the loudness, and it doesn't touch the reed's oscillation (see Reed, below).

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
| Radiation: the tone holes pass the bore's pressure rising 6 dB an octave up to their cutoff (clarinet 700 Hz, oboe 1 kHz) | Chosen by ear | |
| The oboe's formants: peaks of +16 dB at 1305 Hz (Q 5) and +6 dB at 2.9 kHz (Q 2), at Reed 1 | Fitted | The recordings' strongest partials lie at 1.1-1.6 kHz, their fundamentals 8-18 dB under them. With the formants off, the model's harmonics, then the peaks that best turn them into the recordings' (the 1.3 kHz peak); the 2.9 kHz one to their share above 3 kHz. They replace two band-passes set by ear. |
| The cone's correction: the cylinder that stands in for the missing tip resonates at f0 / apex (near the 3rd harmonic) and cancels what the reed sounds there, which a real cone's tip does not. A peak of +12.5 dB at 0.99 f0 / apex (Q 3.2) puts it back | Fitted, with the formants | A4's 3rd harmonic was 12 dB under its fundamental; the recording's is its strongest. Now 5 dB over it. |
| The peaks apply to the reed's steady regime: they come in over 0.15 s as the note settles, and leave as the breath falls away (with the envelope's 4th power) and half-way during a slur's keys | By measurement | The bore's transients carry the cancelled partial already: with the peaks always on, notes ended in a squawk up to 10 dB over the note, attacks rose up to 7 dB over it and slurs up to 6. Now releases stay under the note; C-sharp 4 and D4 still bloom up to 3.5 dB at ff, 0.2-0.35 s in, and slurs up from E5 and E6 blip 4 dB for 10-30 ms. A full slur gate smeared the pitches between the notes (the slur test). |
| The peaks' level: a note whose first two harmonics they reach is lifted; three quarters of that power is taken back | Fitted | To the recordings' levels note by note: the spread around them 5.6 / 4.6 / 5.0 dB (pp / mf / ff) before the peaks, 4.7 / 3.1 / 3.6 now. Taking back only the fundamental's lift left E5 (its 2nd harmonic on the formant) 10 dB over a typical note. |
| Tuning: the oboe's loop runs flat at the top, 4 cents x (f / kHz)^2 taken back | Fitted | Both within 2.5 cents across their ranges at 48 and 96 kHz, pp and ff. |
| Breath noise: turbulence in the mouth's pressure, 20% of a 3 kHz noise, more for the tongue's first 20 ms and while a slur's new resonance takes over | Fitted (the steady part) | The model was 20-35 dB cleaner between its harmonics than the recordings; the oboe is now within 1 dB at mf and 4 at ff, the clarinet within 4-7. More noise slowed the attacks. Breath Noise scales it. |
| The clarinet's even harmonics: its mouthpiece, tone holes and bell are not a perfect cylinder's, and above the tone-hole lattice's cutoff the bore no longer cancels the even harmonics. The radiated pressure's square, normalised to its level and high-passed at 1.5 kHz, 0.3 of it | Phenomenological, amount fitted | The cutoff after Benade (*Fundamentals of Musical Acoustics*). The model's evens were 40-50 dB under the odds at every frequency; the recordings' are 15-30 dB under them at low frequencies and level with them above about 1.5 kHz. An unfiltered even term made low notes' 2nd harmonic far too strong. Spectral error at mf 11.6 to 5.3 dB. |
| Output stage, fitted: the oboe rises 6 dB an octave below 1.13 kHz to a low-pass at 10.5 kHz x (1 + 1.1 b^3); the clarinet darkens at pp, three one-poles at 428 Hz x (1 + 30 b^3) x (f0 / C3)^0.7 | Fitted | The oboe's fundamentals are 8-18 dB under its formant at 1-1.7 kHz in the recordings. |
| Breath wander: the breath is never quite steady, a 6 Hz wander of about 2.5% (oboe) or 3% (clarinet) | Fitted | The model's level was 3-10x steadier than the recordings'; now matched. Seeded: renders repeat. |
| The oboe's tongue releases pressure already built behind it: the breath arrives 5 times as fast as the flute's, with a puff of 0.1 (the flute's 0.35, which a breath this quick would follow into an overshoot). The clarinet keeps the flute's | Fitted | The oboe's recordings speak in 0-35 ms, the model took 50-175 ms: now 20-30 ms behind them at every dynamic, 30-40 before. The clarinet's player tongues softly (50-115 ms), and the flute's attack is within 10 ms of it. |
| The top register, from A5 (oboe) and G5 (clarinet): the oboe's bore losses shrink with its length (the bell's pole scaled by the period's share of A5's), with less of the stretch taken back above A5 (4.4 cents x (f^2 - f_A5^2) / kHz^2); the clarinet's even harmonics grow, 1 + 2 x (octaves above G5)^2 | Fitted | The model's top octave was nearly pure, a flute's: the recordings' 4th-10th harmonics there were 7-12 dB (oboe) and 7-19 dB (clarinet) over the model's, and the clarinet's 2nd often level with its fundamental (the model's 15-26 dB under). Now the oboe's are within 3 dB at every dynamic, the clarinet's within 2 at mf (14 under at pp, where its soft reed keeps it pure). Scaling the losses everywhere overblew the clarinet's top notes to the 3rd harmonic, and made the middle register too rich. The oboe within 2.5 cents across its range. |
| Reed: up to 1 it raises the oboe's formants in dB from none (and the clarinet's even harmonics from none); above 1 they rise a quarter as fast and widen (Q / Reed^1.5), so the buzz spreads rather than picking out the harmonic that lands on a peak. The oboe loses 3 dB a unit of Reed | By ear and measured | Across the range, the oboe's median loudness moves under 1 dB from Reed 0 to 2; at 2 a sharper formant made single notes 15 dB louder. The reed's own constants are no control: its stiffness, opening or the mouth's pressure moved a few percent throw notes into another register or silence them. |

The clarinet sounds D3 to B-flat 6 (the B-flat clarinet's written E3 to C7), straight-toned by
default; the oboe B-flat 3 to A6 with a gentle vibrato (Vibrato Depth 0.6).

## Calibration

Against the University of Iowa Musical Instrument Samples (oboe, Kira Wharton, 1997; B-flat
clarinet, Christine Bellomy, 1998; anechoic, pp, mf and ff): sources and checksums in
`model/iowa-provenance.json`, the analysis in `tools/iowa_calibration.py` (as for the brass,
`docs/brass.md`). Spectral error, mean dB per note, pp / mf / ff:

| | First | Fitted (Oct) | The oboe's formants | The top register |
|---|---|---|---|---|
| Oboe | 6.8 / 8.5 / 7.9 | 6.4 / 7.3 / 7.7 | 6.7 / 5.7 / 6.4 | 6.4 / 5.4 / 6.3 |
| Clarinet | 9.7 / 11.6 / 11.5 | 4.3 / 5.4 / 6.1 | 4.3 / 5.4 / 6.1 | 3.7 / 5.2 / 5.8 |

That score weights each note's strong harmonics, so it hardly saw the top octave's missing upper
harmonics, 35-55 dB down in the recordings but what makes a reed's buzz; the top register's laws were
fitted to those (harmonics 4-10, unweighted).

The oboe's cone takes the reed 0.3-0.35 of the way along the bore, so its tip, a cylinder, cancels
what the reed sounds near the 3rd harmonic, where the real oboe's formant peaks. Its spectrum was a
flute's: the 1st and 2nd harmonics strongest, the 3rd 12-20 dB under them. The fitted peaks put that
back. The exact cone was tried first: its missing tip as a mass of air (an allpass reflection, which
cancels nothing), which made most notes overblow to the octave or the 5th harmonic; a shorter tip made
it worse. The pp shape stays where it was: the recordings' soft notes vary most from note to note.

## Next

- By ear: Reed's range, the low notes' bloom and the slur blips from E5 and E6.
- The oboe's bore: the exact cone (its tip a mass of air), held to its register by a reed with mass
  and the lips' damping, so the peaks need no gate.
- Cor anglais and bassoon: the oboe's cone, longer, with their own formants.
