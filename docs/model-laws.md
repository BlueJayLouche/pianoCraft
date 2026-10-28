# Fitted model laws

`tools/calibrate.py` produced the frozen per-key data in `src/model/CalibrationData.hpp` and
can no longer run: its layer-2 gate closed when `tools/validate_layer2.py` recorded the final
validation. Everything below was fitted afterwards against the same CC0 Meta Piano corpus, using
layers 1, 3 and 4. Layer 2 was only ever scored, never fitted.

`tools/evaluate_model.py` scores the current renderer on every metric named here. With
`--refit` it also re-derives the closed-form laws (decays, key level) and prints them next to
the values in code.

| Constant | Code | Data and method | Result |
|---|---|---|---|
| Prompt decay T60 = 4.89 s (f/440)^-0.745 | `InstrumentModel.cpp` | Early envelope slope (0.05-0.4 s) of partials 1-3, all keys, layer 3; robust power-law fit | Replaces calibrated decays, which were ~1.8x short and barely tracked the data (r=0.28) |
| Aftersound decay T60 = 16.87 s (f/440)^-0.444; partner -18 dB (-35 dB top octave); beat (0.18 + 0.0006 f)(0.6-1.4) Hz | `InstrumentModel.cpp` | Late slope (2-7 s) and late intercept; beat rate from envelope modulation | Sustain error 20 -> ~9 dB |
| Hammer mass 12 -> 6 g, stiffness 8x-32x | `PianoEngine.cpp` | Grid search on octave-band spectra, layers 1/3/4 | Contact 8-15 ms -> 1.5-6 ms |
| Velocity treble shelf, +-8 dB per velocity doubling above 1 kHz | `PianoEngine.cpp` | Grid on 1-8 kHz brightness per layer | pp-ff brightness span within 0.1 dB |
| Bridge high-pass at 0.7 f0 (radiated path only) | `InstrumentModel.cpp` | Low-frequency share of the first ms of the attack | Strike click removed |
| Hammer knock 0.0267 x force into the soundboard | `PianoEngine.cpp` | Between-partial energy 20-100 ms | 8 dB short -> ~1 dB |
| Key level law 0.4 x max(1, f0/64.4)^1.505 | `InstrumentModel.cpp` | Least squares on 88-key RMS levels, layers 1/3/4 (`--refit`). The law also sets the sympathetic drive, so refits wander by a few tenths of a dB rather than converging | Key-to-key error ~17 -> ~4 dB |
| Output high-pass, 3 x one-pole at 100 Hz | `PianoEngine.cpp` | Bass octave bands | Bass fundamentals closer to the recordings |
| Partial extension to 4 kHz along measured inharmonicity | `InstrumentModel.cpp` | Energy above the 32nd partial, A0-E2 | A0's series no longer stops at 1 kHz |
| Soundboard: ~70 shared modes, Q 40 | `Soundboard.cpp` | Between-partial energy 20-300 ms; Q 80 matched averages but rang metallically per note | Modest attack-body gain |
| Spaced-pair phase spread above 100 Hz | `InstrumentModel.cpp` | L/R correlation of the recordings (mean ~0) | Correlation 1.00 -> ~0 |
| Sympathetic output 1.2, drive 1/sqrt(level law), 8 partials, whole board | `SympatheticBank.cpp`, `InstrumentModel.cpp` | Pedal-down between-partial energy 0.3-2.4 s | Within ~5 dB; per-note spread ~4 dB |

The other two mic positions, Coincident and Player, are analytic and have no reference data.
Neither do release or damper noise, which is why they are not modelled.
