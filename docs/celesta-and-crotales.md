# Celesta and crotales

Struck metal for the final act: PianoCraft Celesta and PianoCraft Crotales (Debussy's *cymbales antiques*),
plugins of their own in the bundle, one engine (`StruckEngine`). Each key sets a bar or disc ringing from rest
into its modes, which ring until they die away or are damped. Velocity is the dynamic; CC64 holds off the
dampers, CC11 trims, pitch bend is ±2 semitones, and Timbre moves Brightness per note. The key is the sounding
pitch (the celesta's part is written an octave lower, the crotales' two). Controls: Brightness, Note-Off
Damping, Sustain Length, Master Tune, Stereo Width (0 is exact mono), Output Gain. Each plugin is levelled by its
loudest ff note, as the brass are.

The crotales are fitted to recordings. The celesta's bars are measured on orchestra bells, the nearest
recorded instrument; what makes them a celesta is set by judgment. The clean-room policy applies unchanged.

## Crotales

C6 to C8, the two-octave set. Fitted to the University of Iowa crotales (25 discs, one stroke each at pp and
ff): `tools/iowa_calibration.py modes crotales` measures every stroke and prints the laws below, then renders
the model at the same notes and scores it. x is octaves above C6.

| Law | Source and check |
|---|---|
| Seven modes. Ratio to the fundamental at C6, closing in up the range: 2.14 - 0.12x, 3.74 - 0.28x, 5.72 - 0.50x, 8.03 - 0.85x, 10.50 - 0.95x, 13.46 - 1.89x | Fitted. Within 1-2% of every disc. The top two leave the audio band in the upper octave. |
| Decay: the fundamental's T60 is 13.0 s at C6, x 0.45 an octave (5.9 s at C7, 2.7 s at C8). The 2nd mode's is 10.8 s, x 0.25 an octave; the 3rd's 3.1 s; the rest 0.4-0.6 s | Fitted. Model over recordings: x1.00-1.01 for every mode at ff; each disc varies about x1.25 from the law. |
| Levels at ff, dB re the fundamental: +11.8, +27.6, +28.5, +27.0, +18.8 and +7.5 at C6, each falling up the range. The fundamental itself rises 16 dB an octave | Fitted. Low discs strike bright and settle to their fundamental within a second; high discs are nearly pure. Model error 4.6-6.7 dB a mode at ff, about the discs' own scatter. |
| Each disc differs: its upper modes ±6 dB and their ratios ±1.2%, the same on every render | From the fits' residuals. |
| Beating: the first three modes each pair with a partner 0.4-9 Hz away at 3-25 dB under it, set per disc | The recordings' fundamentals and 2nd modes beat at 0.4-9 Hz, from none to 6 dB of wobble. |
| Dynamics: a mode at f is 8 + 16 log2(f / 1 kHz) dB quieter at pp than at ff | Fitted to whole strokes. ff over pp: recordings 37.2 dB louder and 5.2 dB brighter (share above 3 kHz), model 38.4 and 4.0. Share above 3 kHz at pp -5.4 dB, model -4.5; at ff -0.1, model -0.4. See below. |
| Note-Off Damping 0: the discs ring until they die. At 1, the hand stops every mode in about 0.12 s | |

The modes' own pp levels give a shallower law, 20.5 + 7.43 log2(f / 1 kHz), but that leaves pp strokes 4 dB
too bright. Those levels are the least reliable numbers in the set: weak, fast-dying modes extrapolated back
to a soft stroke, where its click adds to them (the fit left 9.5 dB of scatter). The whole stroke's loudness
and brightness are measured directly, so the model follows them. Against the modes' pp levels it is then
9-16 dB off for modes 2-3 and 25-48 dB for modes 4-7.

The recorded set sits about 17 cents above A = 440. Master Tune places the model.

## Celesta

C4 to C9, the five-octave celesta. Steel bars over wooden resonators, struck by felt hammers, with dampers.
The Iowa collection has no celesta. Its orchestra bells are steel bars too, struck with plastic mallets
(C5-E8, pp, mf and ff; `tools/iowa_calibration.py modes bells`), so they supply the bars.

| Law | Status |
|---|---|
| The bar's fundamental T60: exp(2.608 + 0.127x - 0.491x^2) s, longest near C6 (13.6 s) and shorter both ways (7.3 s at C5, 2.5 s at C8); held at C5's below C5 | Fitted to the bells (×1.33 scatter). |
| The resonator under each bar takes the fundamental's energy faster: 0.4 of the bare bar's T60 (5.4 s at C6, 2.9 s at C5 and below, 1.0 s at C8) | By judgment. |
| Upper partials at 3.11, 6.99, 10.5 and 15.1 times the fundamental, T60 1.0, 0.7, 1.2 and 1.0 s | Fitted to the bells: the medians of the bars that sound them. |
| Their levels: the bells strike them at -16, -7, -20 and -26 dB re the fundamental (in about a quarter of the bars). The felt hammer strikes them 20 dB softer: -36, -27, -40, -46 dB, each bar ±8 dB | Bells fitted; the felt by judgment. |
| Dynamics: every key's fundamental 26 dB from pp to ff; the upper modes soften 16 dB an octave above it, as the crotales' | The bells' fundamentals, ff over pp; the slope from the crotales. |
| Dampers: Note-Off Damping 1 stops every mode in about 0.12 s at note-off; CC64 holds them off | By judgment. |

Not modelled: the action's thump, the resonator boxes' own tone, the felt's wear.

## Next

- By ear: the celesta's resonator decay, its felt and dampers, and the crotales' beating.
- A celesta recording: the bars' upper partials under felt, and the resonators' decay.
- Bowed crotales, if the score asks.
