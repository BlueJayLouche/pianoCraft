# Model laws

`tools/calibrate.py` produced the frozen per-key data in `src/model/CalibrationData.hpp` and
can no longer run: its layer-2 gate closed when `tools/validate_layer2.py` recorded the final
validation. Every constant below came afterwards.

`tools/evaluate_model.py` scores a renderer on every metric named here. `--refit` re-derives
the decay and key-level laws from layers 1, 3 and 4 and the keys outside the every-third-key
split in `model/provenance.json`.

## Evidence status

Layer 2 and the every-third-key split guided earlier iterations and were scored after every
change here, so they are development evidence (`development_exposure` in
`model/provenance.json`). This corpus has no untouched test data left; a final test needs an
independent corpus or new recordings.

Status column:

- **Published**: taken from the named paper, not fitted here.
- **Selected**: hand-chosen against the named metric, which the evaluator reproduces. The
  search itself was not scripted.
- **Analytic**: a modelling choice with no reference data that identifies it.

## Physical structure

- **Hammer:** Stulov's hysteretic felt, F = Q0 (u^p + lambda d(u^p)/dt), the slow-loading form of
  his hereditary model (its relaxation time is microseconds). The felt is solved implicitly
  against the string's give each sample.
- **Strings:** modal, with mass-normalised modes on an analytic string scale. The hammer feels
  the synthesized modes plus the residual static compliance of those not synthesized. The output
  is the bridge force, tension times string slope, in newtons.
- **Unisons:** each partial with a partner is two strings coupled through the bridge (Weinreich
  1977), rendered as their two normal modes with complex shares.
- **Phantom partials:** the squared string slope drives longitudinal modes, with Bank &
  Sujbert's (2005) one-odd-one-even excitation; their bridge force joins the transverse one.

| Constant | Code | Status | Evidence |
|---|---|---|---|
| Hammer set: mass 11.074 - 0.074n + 1e-4 n^2 g, p = 3.7 + 0.015n, Q0 = 183 e^0.045n N/mm^p, hammer n = 1-88 | `InstrumentModel.cpp` | Published | Stulov, Acta Acustica 91 (2005), as fitted in Stulov & Kartofelev (Acoustics High Tatras 2009, eq. 9). The same fit's hereditary amplitude exceeds 1 above n = 10, so it is not used. |
| Felt hysteresis 0.25: lambda = 0.25 u_max / (p x 2 m/s), with u_max the 2 m/s compression against a rigid stop | `InstrumentModel.cpp` | Selected | Swept at the hammer stage with the 500 Hz radiation pole. 0 / 0.25 / 0.5 gave spectrum error 7.42 / 7.23 / 7.94 dB (layer 2: 7.57 / 6.82 / 7.92). |
| String give: 700 N on every string, speaking length 5.2 cm at C8 growing 5.45% a semitone, at most 1.15 m | `InstrumentModel.cpp` | Analytic | 700 N is Bank & Sujbert's value; no recording identifies the scale. Contact times come out about 4.4 ms (C1), 3.2 ms (C4) and 0.8-1 ms (C8), matching Askenfelt's "about 4 ms in the bass to less than 1 ms in the highest treble". |
| Radiation: bridge force through one pole at 500 Hz | `InstrumentModel.cpp` | Selected | Corner 250 / 500 / 1000 / 2000 Hz gave spectrum error 8.02 / 7.94 / 7.97 / 8.82 dB. The old per-partial 1/(1 + 0.018k) weight on modal displacement was about 12 dB/octave darker than bridge force, which the earlier fitted hammer had compensated. |
| Key level law 0.04 x max(1, f0/302.3)^1.042 | `InstrumentModel.cpp` | Selected | Re-fitted once the output became bridge force (which rises with f0). Iterated `--refit` 64.4/1.505 -> 220.0/1.243 -> 303.5/1.093 -> 302.3/1.042; fitting-key std 9.19 -> 3.21 dB. With coupling and phantoms in, the next step (279.2/0.675) lowers that std to 3.14 dB but raises sustain error from 9.07 to 9.82 dB, so it stays. 0.04 keeps the average loudness of the previous voicing. |
| Prompt decay T60 = 4.89 s (f/440)^-0.745; aftersound 16.87 s (f/440)^-0.444 | `InstrumentModel.cpp` | Selected | These now set each coupled unison's two normal modes exactly. Ungated slope fits, partials 1-3, all 88 keys, layer 3. The gated refit (at least 5 dB observed drop, residual at most 3 dB, end at least 15 dB above the band floor, fitting keys only) accepts 44 early and 51 late of 163 partials and gives 2.42 s (f/440)^-0.310 and 22.60 s (f/440)^-0.216. Its window censors long decays. Complete-envelope sustain error at `a472304` (0.25-5 s, layer 3, bass/mid/treble): shipped 9.98 dB (4.8/5.4/13.7), gated 9.84 dB (10.8/4.7/11.4). The shipped laws stay. |
| Coupled unisons fitted to beat (0.18 + 0.0006 f)(0.6-1.4) Hz and slow mode -18 dB (-35 dB top octave) | `InstrumentModel.cpp` | Analytic | Register-wide estimates from aggregate late intercepts and envelope modulation. `calibrate.py` accepted 0 of 106 per-partial secondary candidates (`model/calibration-report.json`), so these are not measured unisons. The inversion is exact: with r + eta fixed by the two decay rates, w = pi beat + i(fast - slow)/2 gives kappa = Im(w^2)/(2 eta), and the mistuning follows from w^2 = coupling^2 + delta^2/4. Eta is bisected for the slow mode's level. Unison Detune scales the mistuning, so at 0 the aftersound is gone. |
| Phantom partials: longitudinal modes at n x 13.06 f0, T60 0.1 s, board coupling 0.03 | `InstrumentModel.cpp`, `PianoEngine.cpp` | Analytic ratio, Selected coupling | E1's strongest peak at ff is the phantom f6 + f7 at 532 Hz, 16 dB above partial 13 beside it: a longitudinal resonance at 13.06 f0. No other key shows one clearly. T60 0.1 s follows Bank & Sujbert's decay time. Coupling 0.015 / 0.03 / 0.05 gave phantom level -5.0 / -0.2 / +3.8 dB re the recordings. The recordings' phantom level rises with velocity relative to the partials (A1 +7 dB pp to ff), and so does the model's (+8 dB). |
| Velocity gain law keeps 0.2 of its slope above mf | `PianoEngine.cpp` | Selected | From mf to ff the recordings rise 4.5 dB and the full law gave 10.1 dB. Velocity span L1-L4, model minus recordings: +6.2 -> -0.8 dB. |
| Velocity treble shelf, +-8 dB per velocity doubling above 1 kHz | `PianoEngine.cpp` | Selected | Rauhala-Välimäki excitation-filter fallback. With the physical hammer, scaling it to 0.6 or 0.3 only traded pp brightness against ff, so it stays at 1. |
| Bridge high-pass at 0.7 f0, radiated path only | `InstrumentModel.cpp` | Selected | Low-frequency share of the first ms of the attack. Onset overshoot mean -0.7 dB, max +4.5 dB. |
| Hammer knock 0.05 x force into the soundboard | `PianoEngine.cpp` | Selected | Between-partial energy 20-100 ms, layer 3: +0.5 dB re the recordings (0.04 gave -1.2, 0.06 +1.2). |
| Output high-pass, 3 x one-pole at 100 Hz | `PianoEngine.cpp` | Selected | Bass octave bands; low fundamentals 4-8 dB closer, C2 still ~16 dB strong. |
| Partial extension to 4 kHz along measured inharmonicity | `InstrumentModel.cpp` | Selected | Energy above the 32nd partial, A0-E2: within ~3 dB on average. |
| Soundboard: ~70 shared modes, Q 40 | `Soundboard.cpp` | Selected | Between-partial energy 20-300 ms. Q 80 matched averages but rang metallically per note (by ear). |
| Spaced-pair phase spread above 100 Hz | `InstrumentModel.cpp` | Selected | L/R correlation, model -0.05 vs recordings -0.01. |
| Sympathetic output 1.2, drive 1/sqrt(level law), 8 partials, whole board | `SympatheticBank.cpp`, `InstrumentModel.cpp` | Analytic | The corpus documents no pedal state, so no pedal coupling is fitted from it (research roadmap 3.4). Pedal-down renders against the isolated-note recordings are a plausibility check only: -1.9/+3.4/-0.6 dB, per-note spread 3.9 dB. Confirm by ear. |
| Coincident and Player mic positions | `InstrumentModel.cpp`, `Soundboard.cpp` | Analytic | No reference recordings. |
| Half-pedal zone 25-75% travel, contact squared | `PianoEngine.cpp` | Analytic | The corpus has no part-pedalled notes; the roadmap defers a measured part-pedal model until there are controlled recordings. Interpolates between the sustain and release poles. |
| Soft pedal: velocity x 0.7 | `PianoEngine.cpp` | Analytic | Upright half-blow action. No soft-pedal recordings. |
| Hammer Hardness h scales Q0 by h^p; Dynamic Range also scales the knock's distance from mf | `PianoEngine.cpp` | Analytic | Control mappings. A linear Q0 scale moved C4's treble share by only ~1 dB over the whole range, because the string's give sets mid-register contact times. |

Release and damper noise are not modelled: the recordings contain no key releases.

## Baseline vs current

`evaluate_model.py` on the first commit (`e1b7e2e`), on `a472304` (before the physical hammer,
coupled unisons and phantom partials) and on this tree. Split keys and layer 2 are development
evidence, as above.

| Metric | Baseline | `a472304` | Current |
|---|---|---|---|
| Spectrum error, layers 1/3/4 | 14.79 dB | 8.93 dB | 8.34 dB |
| Spectrum error, layer 2 | 14.09 dB | 8.22 dB | 8.98 dB |
| Key level std, layer 3: fitting / split keys | 19.06 / 19.14 dB | 3.71 / 4.64 dB | 3.23 / 4.15 dB |
| Worst key levels | A#6 -40.3, C8 -39.4, B6 -37.9 dB | G#6 -15.1, G6 -14.6, A#4 -13.8 dB | A#7 +9.8, G7 +9.5, B7 +8.8 dB |
| Sustain error, layer 3 (0.25-5 s envelope) | 24.16 dB | 9.93 dB | 9.07 dB |
| Worst sustain keys | G#4, G#5, C6 at the 30 dB clip | E7 27.3, G#7 23.6, C5 13.6 dB | G#7 22.4, C5 19.9, C1 9.9 dB |
| L/R correlation (recordings -0.01) | 1.00 | 0.00 | -0.05 |
| Onset overshoot mean / max | +15.2 / +21.1 dB | -0.6 / +4.2 dB | -0.7 / +4.5 dB |
| 1-8 kHz brightness L1 / L2 / L3 / L4 | -15.1 / -15.9 / -21.9 / -26.6 dB | -0.8 / +2.6 / +0.7 / -0.7 dB | -4.7 / +0.6 / -0.1 / -0.4 dB |
| Knock, between-partial 20-100 ms | -0.3 dB | -1.9 dB | +0.5 dB |
| Level at the phantom frequencies, A0-A1, mf/ff | -1.7 dB | -6.4 dB | -0.2 dB |
| Velocity span L1-L4 | +5.9 dB | +6.6 dB | -0.8 dB |
| Halo plausibility, 0.3-0.6 / 0.6-1.2 / 1.2-2.4 s | -3.2 dB, then silent | -3.8 / +2.5 / +1.4 dB | -1.9 / +3.4 / -0.6 dB |

The phantom metric counts any energy at the phantom frequencies, so the baseline's -1.7 dB is
broadband noise, not phantoms. The velocity dependence above is what identifies real ones.

The spectrum metric skips any band where the model is below -90 dB. C5's empty 8-16 kHz band
crossed that gate (-92.3 -> -89.5 dB), which adds a 38 dB deficit to layer 2 that `a472304`
also has. Without the gate, both builds score far worse because bass and mid notes have almost
nothing above 4-8 kHz: layers 1/3/4 14.55 -> 13.85 dB, layer 2 14.40 -> 15.01 dB.

The largest remaining errors are three. The top octave plays about 9 dB loud. G#7's and C5's
sustain is off. Bass and mid notes are missing energy above 4-8 kHz.
