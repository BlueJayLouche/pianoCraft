# Horn, trumpet, trombone and tuba

PianoCraft Horn, Trumpet, Trombone and Tuba share one engine (`src/engine/BrassEngine.cpp`): each note
is a lip-reed waveguide, its own player a cent or two from the others. The lips are a resonance
driven by the pressure across them (Cook's TBone, ICMC 1991), their opening lets the breath into a
bore of one period, and the bell reflects. CC1 or the note's Pressure is breath, velocity shapes only
the attack, CC11 trims, CC64 holds released notes, pitch bend is ±2 semitones. The horn's Stopped is
the others' Straight Mute.

## Calibration

The instruments are fitted to the University of Iowa Musical Instrument Samples: anechoic, mono
chromatic runs at pp, mf and ff, free "for any projects, without restrictions". Sources, licence and
checksums are in `model/iowa-provenance.json`. The recordings stay in `calibration/` (git-ignored)
and are analysis inputs only; the plugin holds just the fitted constants. `tools/iowa_calibration.py`
fetches and checks the recordings, analyses them, renders the model at the same notes (pp, mf and ff
as CC1 0.25, 0.6 and 1.0) and compares, and fits the output filters. Each note is compared by:
- the levels of its first 20 harmonics, as a shape weighted to the strong ones;
- its level, and its brightness (the share above 3 kHz);
- its attack (to within 10 dB of its steady level);
- the noise between its harmonics, and how steady its level is.

Spectral error, mean dB per note:

| | Before, pp / mf / ff | Now, pp / mf / ff | Attack (median, against the recordings) |
|---|---|---|---|
| Horn | 5.3 / 6.7 / 6.5 | 2.8 / 2.4 / 3.1 | 10-25 ms late, now matched |
| Trumpet | 6.8 / 4.4 / 6.8 | 4.6 / 2.6 / 2.4 | 40 ms late at pp, now matched |
| Trombone | 5.6 / 4.0 / 3.4 | 4.6 / 2.8 / 2.5 | 45 ms late at pp, now within 10 ms |
| Tuba | 8.3 / 7.6 / 10.1 | 5.9 / 4.3 / 3.2 | within 10 ms |

The tuba was recorded quietly: harmonics more than 45 dB under its strongest, or above 3 kHz, are
noise and left out of the fit, and its pp is only roughly matched. The remaining error is mostly the
model's own harmonics, which the filters cannot reach: a weak 3rd harmonic beside the 4th, and upper
notes richer at pp than a player's.

## The laws

| Law | Status | Source and check |
|---|---|---|
| Compass: F horn B-flat 1-F5, B-flat trumpet E3-C6, tenor trombone E2-F5, tuba D1-F4 | Standard ranges | Notes outside are refused. |
| The bell's formant: a Butterworth high-pass, third-order at 448 Hz (horn), 1.72 kHz (trumpet) or 483 Hz (tuba), second-order at 967 Hz (trombone) | Fitted | Radiation weakens steeply below a bell's cutoff. The horn's old one-pole at 500 Hz left its low notes dominated by their fundamentals, hollow and thin, where the recordings' fundamentals are 15-18 dB under the harmonics around 300-500 Hz. The low horn's spectral centroid was 110-280 Hz against the recordings' 420-570, and is now 410-550. The trumpet's formant matches its known one near 1.5 kHz (Luce and Clark, JASA 42, 1967). Test: a low horn note's fundamental at least 10 dB under its strongest harmonic. |
| The tone: three one-pole low-passes at tone x Brightness x (1 + bloom x breath^3) x (f0 / C3)^track. Horn 850 Hz, bloom 3.17, track -0.22 (the hand in the bell). Trumpet 1.44 kHz, no bloom. Trombone 1.17 kHz, bloom 3.22. Tuba 416 Hz, bloom 3.01, track -1 | Fitted | The recordings are nearly pure at pp and mf and open out at ff, far more than the model's lips alone. The horn's low-pass runs about 0.9 kHz at pp, 1.4 at mf and 3.5 at ff. Upper notes are purer than lower ones at the same frequencies, hence the pitch tracking. The trumpet's crescendo is its blare's. |
| Blare: at loud dynamics the outgoing wave's crests run ahead of its troughs and steepen towards shocks, so the bell radiates the wave read at a delay shortened by 2.5 (horn), 4 (trumpet) or 5 (trombone) samples x pressure x breath^2 | Analytic form, amounts fitted | Nonlinear propagation in the bore (Hirschberg, Gilbert, Msallam and Wijnands, JASA 99, 1996); delay-line form after Vergez and Rodet (2000). The horn at ff was 10-20 dB short at 5-6 kHz, its cuivré edge; now within about 1 dB above 3 kHz. More than about 6 samples folded high notes' waves. |
| The attack: the tongue's release launches the note's wave, so the bore starts with a sinusoid of the note's period at 1.2-2 x (0.3 + 0.7 velocity) x (0.35 + 0.65 breath), and the breath arrives in a half to a third of the old time | Fitted | The lips' buzz grows about 1 dB a millisecond from nothing, however fast the breath arrives: a mid horn note took 35-45 ms to reach its level, where the recordings' jump to within 10 dB in 5-10 ms, then settle within 15-30 ms with no overshoot. Pulses into the bore only clicked, dipped and grew as slowly. The fundamental leads and the upper harmonics follow within a few periods, as in brass. Test: a horn note speaks within 15 ms. |
| Breath: turbulence (noise below 2 kHz, about 1.5% rms) and wander (under 6 Hz, about 2% rms) in the mouth's pressure | Fitted | The model was 45 dB cleaner between its harmonics than the recordings, and its level 4x steadier. Now within about 5 dB and matched. Seeded per note: renders repeat exactly. |
| The swell: the level x (breath / 0.6)^swell, 0.5 for the trombone and 0.3 for the tuba | Fitted | The recordings' pp to ff is wider than the lips give: trombone pp was 5 dB too loud, tuba pp 6 dB too loud and ff 8 dB too soft. Mean level per dynamic now within 2 dB. |
| The stopped horn: the hand closes the bell, replacing its tone with a 2.8 kHz resonance (Q 3), blown 20% harder | Chosen by ear (no recordings) | About 4 dB quieter and far thinner. |
| Straight mute: in a bell that keeps its tone, a resonance at 3.5 kHz (trumpet), 2 kHz (trombone) or 1 kHz (tuba), Q 1.5, over 0.3 of the sound | Chosen by ear (no recordings) | 5-6 dB quieter and 5-6 dB thinner. |
| Bell reflection: a one-pole low-pass in the loop (2.5 kHz horn, 5 kHz trumpet, 3.5 kHz trombone, 1.5 kHz tuba), its phase delay taken out of the tuning; lips at 0.8 of the note pull it 17 cents sharp, taken back | Fitted | Within 6 cents across each compass at 48 and 96 kHz. |
| Voice stacking (Bitwig plays one key as several notes): each stacked voice is another player, 3-8 cents from the first (sharp and flat in turn), wandering 1.5-2.5 cents at 0.15-0.4 Hz, coming in up to 20 ms later, a little apart in the image, its own breath. The first plays as an unstacked note | Chosen by ear | Seeded by key and place in the stack: the same every render. A stacked voice correlates under 0.1 with the first (a copy, as before, 1); within a cent of it, two stacked horns stayed phase-locked for seconds, partly cancelling. |
| Level: each instrument scaled so its loudest ff note peaks near 0.7 | Measured on the model | Within an instrument the levels follow the recordings by note and dynamic, within about 2 dB at mf and ff. So the trombone's low notes at mf are 15-20 dB under its top, as recorded; Output Gain and CC11 trim. |

Eight held notes cost about 0.5% of real time at 48 kHz.

## Next

- By ear: the blare's amount, the mutes and the stopped horn (no recordings), and the tuba's pp.
- The model's own harmonics: the 3rd harmonic's dip and the upper notes' purity at pp are in the lips,
  not the filters.
- A second corpus to check against, so these fits are not one player's and one room's.
- Cup and Harmon mutes, if the score asks.
