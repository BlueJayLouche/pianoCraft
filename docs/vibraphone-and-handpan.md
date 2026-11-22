# Vibraphone and handpan

Two more struck-metal plugins on `StruckEngine`: PianoCraft Vibraphone and PianoCraft Handpan. They work as the
celesta and crotales do (`docs/celesta-and-crotales.md`). Velocity is the dynamic, CC11 trims, pitch bend is ±2
semitones, and Timbre moves Brightness per note. The key is the sounding pitch. Controls: Brightness, Note-Off
Damping, Sustain Length, Master Tune, Stereo Width (0 is exact mono) and Output Gain. The vibraphone also has
Motor Speed and Motor Depth. Each plugin is levelled by its loudest ff note, as the celesta and crotales are.

The vibraphone is fitted to recordings. The handpan has no corpus, so it is set by judgment. The clean-room policy
applies unchanged.

## Vibraphone

C3 to F6, the 3½-octave vibraphone. It is fitted to the University of Iowa vibraphone: one stroke per bar at pp, mf
and ff, pedal down, motor off. Only the C5-B5 run splits at pp, and C6-F6 doesn't split at mf; the others are too
faint or have a stray knock. `tools/iowa_calibration.py modes vibraphone` measures every stroke and prints the laws
below. x is octaves above C6, so it is negative over most of the instrument.

| Law | Source and check |
|---|---|
| The fundamental's T60: exp(2.451 - 0.176x - 0.006x^2) s. That is 18.6 s at C3, 13.8 s at C5 and 10.8 s at F6 | Fitted, x1.32 scatter. Model over recordings at ff: x0.95. |
| The fundamental's ff level rises 2.8 dB an octave | Fitted. The model's per-key level is within 3.1 dB rms of the recordings. |
| The tuned 4th: ratio 3.93 - 0.03x (4.00 in nearly every bar; the top bars fall to 3.75-3.95). Level -29.1 - 2.7x dB, T60 exp(-0.149 - 0.683x): 6.7 s at C3, 0.65 s at F6 | Fitted, in 36 of 42 bars. |
| The tuned 10th: ratio 9.79 - 0.08x. Level -48.0 - 18.3x dB, which is +7 dB over the fundamental at C3 and -21 dB at F#4. T60 0.9 s at C3, 0.5 s at F#4 | Fitted, in 23 of 42 bars. Above A4 no bar shows it within 50 dB of the fundamental. |
| A torsional mode falling through them: ratio 2.04 - 2.64x (10.0 at C3, 5.1 at B-flat 4). Level -61.0 - 11.7x dB (-26 at C3), T60 3.6 s at C3 | Fitted, in 19 of 42 bars, traced up to B-flat 4. |
| The 4th bending mode: ratio 10.16 - 1.97x (16.1 at C3, 14.1 at C4). Level -100.6 - 32.7x dB (-2.5 at C3, -35 at C4), T60 4.1 s at C3, 1.1 s at C4 | Fitted, in 15 of 42 bars. It is strong only in the lowest bars. |
| Each bar differs: its upper modes' levels by ±10 dB and their ratios by ±0.3%, the same on every render | The recordings' modes scatter ±10-11 dB rms about their laws (some of that is measurement). The tuned 4th stays at 4.00, so the ratios scatter little. |
| Dynamics: the fundamental changes 30 dB from pp to ff. The upper modes soften a further 8.4 dB an octave above it | The softening is fitted, ff over mf: 3.6 dB an octave at mf, over 67 modes. The 30 dB is by judgment. In the recordings mf is 8 dB under ff and pp is 46 dB under, and no single velocity law meets both. Test: ff over pp is 30 dB louder and 20 dB brighter. |
| Dampers: Note-Off Damping 1 stops every mode in about 0.12 s at note-off. CC64 holds them off | As the celesta. |
| The motor: Motor Speed runs 0-12 Hz; at 0 the motor stops with the discs open. Each disc turns in the top of a resonator, and the resonators are tuned to the fundamental, so only the fundamental swells. Motor Depth d sets its level between 1 and 1 - d (-10.5 dB at the default 0.7) | By judgment: the recordings have no motor (their fundamentals wobble 0.1 dB rms). Test: at 4 Hz the fundamental swings 12.5 dB, against 3.2 dB of plain decay with the motor off, and the 4th mode doesn't swing. |
| A note struck into silence starts the motor's phase from zero | So a passage that starts from silence renders the same every time (need 4 in `docs/old-house-needs.md`). Test. |

The recordings also show exact 2nd and 3rd harmonics 40 dB down. They decay at 2 and 3 times the fundamental's rate,
so they are distortion, not modes, and the model leaves them out.

The recorded set is tuned about 10 cents above A = 440 (A = 442.5). Master Tune places the model.

## Handpan

C3 to C6, chromatic: every key is a tone field. A real handpan has eight or nine in one scale (D Kurd: D3, A3,
B-flat 3, C4, D4, E4, F4, G4, A4), so play the notes of the one you mean. There is no recording, so all of it is set
by judgment.

| Law | Status |
|---|---|
| Each tone field's first three modes are tuned 1:2:3: fundamental, octave and compound fifth | The makers' practice, as Rossing and colleagues measured on the Hang. |
| Levels at ff, re the fundamental: the octave -6 dB, the fifth -14 dB. Each field varies ±3 dB, its ratios ±0.2% (3 cents) | By judgment. |
| T60: the fundamental's is exp(1.6 - 0.35x) s, with x octaves above D4: 7.5 s at C3, 5.0 s at D4, 2.6 s at C6. The octave's is 0.6 of it, the fifth's 0.3 | By judgment. |
| The octave and the fifth each beat with a twin 0.4-1.5 Hz away and 3-25 dB under it. The fundamental is clean | By judgment, on the crotales' beating law. |
| Dynamics: the fundamental changes 24 dB from pp to ff, and the upper modes soften 10 dB an octave more | By judgment. Test: ff over pp is 25 dB louder and 8 dB brighter. |
| Notes ring on after note-off. Note-Off Damping 1 is the hand stopping them in about 0.12 s | |

Not modelled: the finger's knock, the shell's cavity (Helmholtz) resonance under the ding, the tone fields ringing
each other in sympathy, and the octave's swell after the strike.

## Next

- By ear: the motor's depth, and the handpan's levels and decays.
- A handpan recording, with its licence recorded, to fit as the vibraphone was.
- If the score asks: bowed vibraphone (the Iowa set has it) and dead strokes.
