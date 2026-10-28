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

- **Selected**: hand-chosen against the named metric, which the evaluator reproduces. The
  search itself was not scripted.
- **Analytic**: a modelling choice with no reference data that identifies it.

| Constant | Code | Status | Evidence |
|---|---|---|---|
| Prompt decay T60 = 4.89 s (f/440)^-0.745; aftersound 16.87 s (f/440)^-0.444 | `InstrumentModel.cpp` | Selected | Ungated slope fits, partials 1-3, all 88 keys, layer 3. The gated refit (at least 5 dB observed drop, residual at most 3 dB, end at least 15 dB above the band floor, fitting keys only) accepts 44 early and 51 late of 163 partials and gives 2.42 s (f/440)^-0.310 and 22.60 s (f/440)^-0.216. Its window censors long decays. Complete-envelope sustain error (0.25-5 s, layer 3, bass/mid/treble): shipped 9.98 dB (4.8/5.4/13.7), gated 9.84 dB (10.8/4.7/11.4). The shipped laws stay. |
| Unison partners: -18 dB (-35 dB top octave), beat (0.18 + 0.0006 f)(0.6-1.4) Hz | `InstrumentModel.cpp` | Analytic | Register-wide estimates from aggregate late intercepts and envelope modulation. `calibrate.py` accepted 0 of 106 per-partial secondary candidates (`model/calibration-report.json`), so these are not measured unisons. |
| Hammer mass 12 -> 6 g, stiffness 8x-32x | `PianoEngine.cpp` | Selected | Effective excitation parameters from a grid on octave-band spectra, layers 1/3/4. Radiated audio cannot separate mass, stiffness and strike position. Contact time 8-15 ms -> 1.5-6 ms. |
| Velocity treble shelf, +-8 dB per velocity doubling above 1 kHz | `PianoEngine.cpp` | Selected | Rauhala-Välimäki excitation-filter fallback, chosen after a velocity-stiffness law gave erratic per-note spectra. 1-8 kHz brightness error L1 -0.7, L3 +0.7, L4 -0.7, L2 +2.7 dB. |
| Bridge high-pass at 0.7 f0, radiated path only | `InstrumentModel.cpp` | Selected | Low-frequency share of the first ms of the attack. Onset overshoot mean -0.6 dB, max +4.3 dB. |
| Hammer knock 0.0267 x force into the soundboard | `PianoEngine.cpp` | Selected | Between-partial energy 20-100 ms: 8 dB short -> within ~1 dB. |
| Key level law 0.4 x max(1, f0/64.4)^1.505 | `InstrumentModel.cpp` | Selected | `--refit` is a one-step linear correction. On fitting keys it suggests 62.3/1.444; built and rescored, that moves its objective (layers 1/3/4, fitting-key std) from 3.52 to 3.47 dB and refits again to 60.9/1.417. It does not converge because the knock and sympathetic drive don't scale with the law, and 0.05 dB is within noise, so 64.4/1.505 stays. |
| Output high-pass, 3 x one-pole at 100 Hz | `PianoEngine.cpp` | Selected | Bass octave bands; low fundamentals 4-8 dB closer, C2 still ~16 dB strong. |
| Partial extension to 4 kHz along measured inharmonicity | `InstrumentModel.cpp` | Selected | Energy above the 32nd partial, A0-E2: within ~3 dB on average. |
| Soundboard: ~70 shared modes, Q 40 | `Soundboard.cpp` | Selected | Between-partial energy 20-300 ms. Q 80 matched averages but rang metallically per note (by ear). |
| Spaced-pair phase spread above 100 Hz | `InstrumentModel.cpp` | Selected | L/R correlation, model 0.00 vs recordings -0.01. |
| Sympathetic output 1.2, drive 1/sqrt(level law), 8 partials, whole board | `SympatheticBank.cpp`, `InstrumentModel.cpp` | Analytic | The corpus documents no pedal state, so no pedal coupling is fitted from it (research roadmap 3.4). Pedal-down renders against the isolated-note recordings are a plausibility check only: -3.8/+2.5/+1.4 dB, per-note spread 3.8 dB. Confirm by ear. |
| Coincident and Player mic positions | `InstrumentModel.cpp`, `Soundboard.cpp` | Analytic | No reference recordings. |

Release and damper noise are not modelled: the recordings contain no key releases.

## Baseline vs current

`evaluate_model.py` on the first commit (`e1b7e2e`) and on this one. Split keys and layer 2 are
development evidence, as above.

| Metric | Baseline | Current |
|---|---|---|
| Spectrum error, layers 1/3/4 | 14.79 dB | 8.95 dB |
| Spectrum error, layer 2 | 14.09 dB | 8.21 dB |
| Key level std, layer 3: fitting / split keys | 19.06 / 19.14 dB | 3.71 / 4.62 dB |
| Worst key levels | A#6 -40.3, C8 -39.4, B6 -37.9 dB | G#6 -15.1, G6 -14.5, A#4 -13.7 dB |
| Sustain error, layer 3 (0.25-5 s envelope) | 24.16 dB | 9.98 dB |
| Worst sustain keys | G#4, G#5, C6 at the 30 dB clip | E7 27.7, G#7 23.6, C5 13.6 dB |
| L/R correlation (recordings -0.01) | 1.00 | 0.00 |
| Onset overshoot mean / max | +15.2 / +21.1 dB | -0.6 / +4.3 dB |
| 1-8 kHz brightness L1 / L2 / L3 / L4 | -15.1 / -15.9 / -21.9 / -26.6 dB | -0.7 / +2.7 / +0.7 / -0.7 dB |
| Halo plausibility, 0.3-0.6 / 0.6-1.2 / 1.2-2.4 s | -3.2 dB, then silent | -3.8 / +2.5 / +1.4 dB |

The treble's sustain (E7, G#7) and key level (G6, G#6) are the largest remaining errors.
