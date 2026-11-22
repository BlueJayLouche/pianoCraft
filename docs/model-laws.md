# Model laws

`tools/calibrate.py` produced the frozen per-key data in `src/model/CalibrationData.hpp` and
can no longer run: its layer-2 gate closed when `tools/validate_layer2.py` recorded the final
validation. Every constant below came afterwards. `tools/fit_piano.py` fits the strike positions,
radiation, velocity and decay tables in `src/model/SpectrumCalibration.hpp` (see "Fitted spectrum and
decays" below).

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
- **Fitted**: measured per key or per partial from the recordings by `tools/fit_piano.py`, which
  regenerates it (`neutral`, `strike`, `decays`, `spectrum 6`).

## Physical structure

- **Hammer:** Stulov's hysteretic felt, F = Q0 (u^p + lambda d(u^p)/dt), the slow-loading form of
  his hereditary model (its relaxation time is microseconds). The felt is solved implicitly
  against the string's give each sample.
- **Strings:** modal, with mass-normalised modes on an analytic string scale. The hammer feels
  the synthesized modes plus the residual static compliance of those not synthesized. The output
  is the bridge force, tension times string slope, in newtons.
- **Unisons:** each partial with a partner is two strings coupled through the bridge (Weinreich
  1977), rendered as their two normal modes with complex shares.
- **Board build-up:** each new strike's sound rises as the board's modes ring up, over Q / (pi f) at
  Q 100, at most 10 ms; a string already ringing sounds on through it.
- **Phantom partials:** the squared string slope at the bridge drives longitudinal modes (Bank &
  Sujbert 2005), so every pair of partials makes one; their bridge force joins the transverse one.

| Constant | Code | Status | Evidence |
|---|---|---|---|
| Hammer set: mass 11.074 - 0.074n + 1e-4 n^2 g, p = 3.7 + 0.015n, Q0 = 183 e^0.045n N/mm^p, hammer n = 1-88 | `InstrumentModel.cpp` | Published | Stulov, Acta Acustica 91 (2005), as fitted in Stulov & Kartofelev (Acoustics High Tatras 2009, eq. 9). The same fit's hereditary amplitude exceeds 1 above n = 10, so it is not used. |
| Felt hysteresis 0.25: lambda = 0.25 u_max / (p x 2 m/s), with u_max the 2 m/s compression against a rigid stop | `InstrumentModel.cpp` | Selected | Swept at the hammer stage with the 500 Hz radiation pole. 0 / 0.25 / 0.5 gave spectrum error 7.42 / 7.23 / 7.94 dB (layer 2: 7.57 / 6.82 / 7.92). |
| String give: 700 N on every string, speaking length 5.2 cm at C8 growing 5.45% a semitone, at most 1.15 m | `InstrumentModel.cpp` | Analytic | 700 N is Bank & Sujbert's value; no recording identifies the scale. Contact times come out about 4.4 ms (C1), 3.2 ms (C4) and 0.8-1 ms (C8), matching Askenfelt's "about 4 ms in the bass to less than 1 ms in the highest treble". |
| Radiation: bridge force through one pole at 500 Hz, then the fitted radiation and each key's own partials (below) | `InstrumentModel.cpp` | Selected | Corner 250 / 500 / 1000 / 2000 Hz gave spectrum error 8.02 / 7.94 / 7.97 / 8.82 dB. The old per-partial 1/(1 + 0.018k) weight on modal displacement was about 12 dB/octave darker than bridge force, which the earlier fitted hammer had compensated. |
| Key level law 0.04 x max(1, f0/302.3)^1.042 | `InstrumentModel.cpp` | Selected | Re-fitted once the output became bridge force (which rises with f0). Iterated `--refit` 64.4/1.505 -> 220.0/1.243 -> 303.5/1.093 -> 302.3/1.042; fitting-key std 9.19 -> 3.21 dB. With coupling and phantoms in, the next step (279.2/0.675) lowers that std to 3.14 dB but raises sustain error from 9.07 to 9.82 dB, so it stays. 0.04 keeps the average loudness of the previous voicing. |
| Prompt decay T60 = 4.89 s (f/440)^-0.745; aftersound 16.87 s (f/440)^-0.444, each now times a fitted factor per partial, and the slow mode's level per partial (below) | `InstrumentModel.cpp` | Selected | These now set each coupled unison's two normal modes exactly. Ungated slope fits, partials 1-3, all 88 keys, layer 3. The gated refit (at least 5 dB observed drop, residual at most 3 dB, end at least 15 dB above the band floor, fitting keys only) accepts 44 early and 51 late of 163 partials and gives 2.42 s (f/440)^-0.310 and 22.60 s (f/440)^-0.216. Its window censors long decays. Complete-envelope sustain error at `a472304` (0.25-5 s, layer 3, bass/mid/treble): shipped 9.98 dB (4.8/5.4/13.7), gated 9.84 dB (10.8/4.7/11.4). The shipped laws stay. |
| Coupled unisons fitted to beat (0.18 + 0.0006 f)(0.6-1.4) Hz and slow mode -18 dB (-35 dB top octave) | `InstrumentModel.cpp` | Analytic | Register-wide estimates from aggregate late intercepts and envelope modulation. `calibrate.py` accepted 0 of 106 per-partial secondary candidates (`model/calibration-report.json`), so these are not measured unisons. The inversion is exact: with r + eta fixed by the two decay rates, w = pi beat + i(fast - slow)/2 gives kappa = Im(w^2)/(2 eta), and the mistuning follows from w^2 = coupling^2 + delta^2/4. Eta is bisected for the slow mode's level. Unison Detune scales the mistuning, so at 0 the aftersound is gone. |
| Phantom partials: longitudinal modes at n x 13.06 f0, T60 0.1 s, board coupling 0.03 | `InstrumentModel.cpp`, `PianoEngine.cpp` | Analytic ratio, Selected coupling | E1's strongest peak at ff is the phantom f6 + f7 at 532 Hz, 16 dB above partial 13 beside it: a longitudinal resonance at 13.06 f0. No other key shows one clearly. T60 0.1 s follows Bank & Sujbert's decay time. Coupling 0.015 / 0.03 / 0.05 gave phantom level -5.0 / -0.2 / +3.8 dB re the recordings. The recordings' phantom level rises with velocity relative to the partials (A1 +7 dB pp to ff), and so does the model's (+8 dB). Driven by neighbouring partials only (k with k+1 and k+2, Bank & Sujbert's simplification), each order made one phantom where Meta's forte bass has a cluster at f_j + f_k spread by inharmonicity (E1 at velocity 120: phantoms within 0-3 Hz of pair sums, 8-27 Hz off the partials, through 0.8-1.7 kHz). The squared slope at the bridge pairs every partial and is cheaper: A#0-A#1's energy between partials (0.4-4 kHz, 0.03-0.33 s) -31 / -30 dB re the partials at velocity 40 / 120 to -30 / -26 (Meta -22 / -13). Without the 500 Hz pole the longitudinal force rises with n unchecked: C2's 2-4 kHz octave at ff went 23 dB over Meta. |
| Velocity gain law keeps 0.2 of its slope above mf | `PianoEngine.cpp` | Selected | From mf to ff the recordings rise 4.5 dB and the full law gave 10.1 dB. Velocity span L1-L4, model minus recordings: +6.2 -> -0.8 dB. |
| Velocity treble shelf above 1 kHz: 8 dB per velocity doubling above mf, 20 dB per halving below. Now on the hammer knock only: the strings take the fitted velocity tables (below) | `PianoEngine.cpp` | Selected | Rauhala-Välimäki excitation-filter fallback, re-selected under the reference-floor gate. Below mf, 8 / 14 / 20 / 24 / 28 dB gave layers 1/3/4 spectrum error 6.53 / 6.28 / 6.17 / 6.17 / 6.20 dB and L1 brightness +5.2 / +1.7 / 0.0 / -0.2 / -0.1 dB. One slope both ways can't do it: 12 or 16 brightened ff (L4 +1.3, +3.0 dB). |
| Bridge high-pass at 0.7 f0, radiated path only | `InstrumentModel.cpp` | Selected | Low-frequency share of the first ms of the attack. Onset overshoot mean -0.7 dB, max +4.5 dB. |
| Hammer knock 0.03 x force into the soundboard, through the velocity shelf and two poles at 16 f0, at most 4 kHz: about 17 dB under the strings over a mf C4's first 100 ms. The original 0.05 and open low-pass are described in this row's evidence; by ear (20 Nov 2026) the knock was far too loud, as the metric below counts the recordings' room and noise between partials too (it now reads -6 dB). Above C5 the open low-pass clicked: notes peaked in 2-4 ms and fell 10-15 dB by 30 ms where the recordings rise over 10 ms | `PianoEngine.cpp` | Selected | Between-partial energy 20-100 ms, layer 3: +0.4 dB re the recordings (0.04 gave -1.2, 0.06 +1.2). Unshelved, it gave pp attacks their bright click: attack 2-8 kHz share L1 +17.6 -> +0.9 dB. Unfiltered, it was most of C2's and C3's attack excess at every velocity, while C5 and C6 need its treble. A fixed corner darkened the treble's pp attacks too; 8 f0 one pole / 8 f0 two / 16 f0 two / 32 f0 two gave attack share L1-L4 means -3.6..+4.1 / -5.8..+2.3 / -3.7..+3.4 / -0.8..+6.2 dB, and 16 f0 left C3 and C4 within 2.3 dB of the recordings at every layer. Also scaling it by the velocity gain law evened its level error across layers (L1/L4 +3.1/-1.4 -> -1.8/-0.5 dB) but left pp 5 dB too quiet (velocity span +4.8 dB), so it doesn't. |
| The knock falls a further 10 dB per halving of velocity below mf, all of it | `PianoEngine.cpp` | Selected | Scaled by force alone, a soft note's knock stood out: its first 16 ms, aligned on the peak, sat 5-7 dB hotter over its 0.1-0.5 s level than Meta's (velocity 40, 11 keys: +18.9 / +12.7 / +11.4 dB for the peak 1 ms, first 4 ms and 4-16 ms; Meta +11.7 / +7.3 / +5.0). 10 dB: +14.3 / +9.3 / +8.3; 14 dB barely further. Spectrum error at velocity 40 also improves (4.27 to 4.09 dB). Test: `strikesBuild`. |
| Below mf the knock also falls 6 dB an octave above C4, in proportion (all of it at velocity 40, none from mf) | `PianoEngine.cpp` | Selected | At its C4 level the treble's knock was most of a soft note's first 10 ms (C6 at velocity 40: -1.3 dB over its body without it, +5.1 with, Meta -6.7), 4 dB hotter than Meta's above C5. Falling at every velocity, the top octave's level, which rests on it, dropped 10-12 dB (G#7-B7); 3 and 8 dB an octave scored worse. |
| Attack sheen: the hammer's force x noise through two one-pole high-passes at 5 kHz, into the voice's radiated output while the hammer is on the string (the right channel mixes a second noise in, 0.6 / 0.8: Meta's attacks above 4 kHz correlate 0.3-0.9), 0.006 x 2^(-(key - 60) / 24) | `PianoEngine.cpp` | Selected | The strings' partials above 9 kHz, which aren't synthesized. Share of the first 6 ms above 4 kHz, model - Meta, velocity 40 / 80 / 120, 16 keys: -8.1 / -4.8 / -5.2 dB, now -2.7 / +1.0 / -1.0 (0.02: -0.9 / +2.6 / +0.3; level alone left the bass 2-9 dB short and the treble 3-13 over, hence the 3 dB an octave). Key-to-key rms 8.4 to 4.9 dB. 20-60 ms unchanged. With the strike's burst gone (below) 0.015 overshot by 3-5 dB; 0.006 gives +1.1 / +0.4 / -0.1 dB. |
| Dispersion: each partial reaches the microphones with its own phase (a hash of its frequency, shared by its coupled modes, the same in both channels and in the bridge's drive of the board) | `InstrumentModel.cpp` | Analytic | The board is dispersive and radiates each partial from all over it. Without it every mode started in phase at the strike: chords peaked 4-6 dB higher. Rotating the direct sound alone set the board's share of each partial at a random phase to it and cost the octave spectra 0.7 dB; with the drive rotated too, spectrum error is 3.72 / 3.23 dB (fitting layers / held-out layer 2) before refitting the tables, 4.05 / 3.52 after, from 4.21 / 3.75. The halo plausibility check drops 5-8 dB with it (-6.0 / -5.2 / -13.8), but the A/B project's pedalled sections change under 0.5 dB: the check counts energy off the note's calibrated partials in L+R, which their phases move. |
| The fitted lifts reach a strike's own motion only once the hammer has left: while it is on the string, that motion radiates without the radiation and per-partial tables' gains above 0 dB, which come in over 8 ms after it leaves; a restruck string's ringing keeps them throughout | `PianoEngine.cpp`, `InstrumentModel.cpp` | Selected | The tables lift weak high partials up to 40 dB where the felt excites them too little, and while the hammer is on the string those modes follow its push: lifted, that push was a burst. C6 at velocity 120, first 4 ms over its body, 22 to 16 dB (Meta ~9; the rest is its early decay). Over 11 keys at velocity 120 the sample peak in the first 10 ms over the body goes from +17.3 to +13.3 dB (Meta +10.4), and the 1 ms frames sit within 1.5 dB of Meta's; 2 and 4 ms rises left the peak 1.5-2 dB higher. Test: `strikesBuild`. |
| Board build-up: each strike's own motion radiates times 1 - e^(-t / tau), tau = Q / (pi f) at Q 100, at most 10 ms; a restruck string's ringing sounds on through it | `InstrumentModel.cpp`, `PianoEngine.cpp` | Selected | A board mode takes Q / (pi f) to ring up. Meta's partials rise over a near-constant 10-14 ms to ~3 kHz (192 onsets at velocity 60 and 80; pi f tau 20 at 300-600 Hz, 30 at 0.6-1.2 kHz, 60 at 1.2-3 kHz): C4's fundamental starts 14 dB under its 80 ms level. Struck at full level, the treble's first 10 ms stood 8-11 dB over its body against Meta's and peaked 7-11 ms early. Scored on the first 10 ms over the 50-150 ms body, the 1 ms peak and the rise time by register at velocity 40 and 120, and the attack's 2-8 kHz share: one time constant for every mode (8-12 ms) left the bass's first 10 ms 2-4 dB short; Q 30 capped at 6 / 10 / 15 ms and Q 20 scored worse; Q 30 built 2-3 kHz three times too fast, the attack 2 dB bright; Q 300 scored as 100. Built on the whole string, a soft restrike of a ringing C4 dipped 17 dB (`boardBuilds`). |
| Output high-pass, 3 x one-pole at 100 Hz | `PianoEngine.cpp` | Selected | Bass octave bands; low fundamentals 4-8 dB closer, C2 still ~16 dB strong. |
| Partial extension to 9 kHz along measured inharmonicity (was 4 kHz) | `InstrumentModel.cpp` | Fitted | At 4 kHz the bass's and mid's series stopped in a visible wall; the recordings hold partials to 6-9 kHz, which the fitted levels now place. |
| Soundboard: ~70 shared modes, Q 40 | `Soundboard.cpp` | Selected | Between-partial energy 20-300 ms. Q 80 matched averages but rang metallically per note (by ear). |
| Spaced-pair phase spread above 100 Hz, one per partial (its two coupled modes share it) | `InstrumentModel.cpp` | Selected | L/R correlation, model -0.05 vs recordings -0.01. |
| Sympathetic output 1.2, drive 1/sqrt(level law), 8 partials, whole board | `SympatheticBank.cpp`, `InstrumentModel.cpp` | Analytic | The corpus documents no pedal state, so no pedal coupling is fitted from it (research roadmap 3.4). Pedal-down renders against the isolated-note recordings are a plausibility check only: -1.9/+3.4/-0.6 dB, per-note spread 3.9 dB. Confirm by ear. |
| Coincident and Player mic positions | `InstrumentModel.cpp`, `Soundboard.cpp` | Analytic | No reference recordings. |
| Half-pedal zone 25-75% travel, contact squared | `PianoEngine.cpp` | Analytic | The corpus has no part-pedalled notes; the roadmap defers a measured part-pedal model until there are controlled recordings. Interpolates between the sustain and release poles. |
| Soft pedal: velocity x 0.7 | `PianoEngine.cpp` | Analytic | Upright half-blow action. No soft-pedal recordings. |
| Age at 1: pitch -35 cents +-6 per key; unisons drifted 6-38 cents apart (median 14), added in cents to Unison Detune's; damper release 1.5x (up to 5.5x on a few keys), hammers 15% harder +-35%, key response +-2.5 dB; a fixed per-key hash | `InstrumentModel.cpp` | Selected (pitch, unisons), Analytic (the rest) | The old upright recorded for the score (22 Nov 2026), C4-A4: about 35 cents flat (A0 77), unisons C4 15, G4 11, A4 18, F4 38 cents with the second string 1-3 dB under the first. Age 1 now plays those keys 10-28 cents apart, within 0-1 dB; it was 1-3 cents (1.5-4.5x a beat law that grows toward the bass in cents, so scaling it would have put C2 near 80). Two modes per partial, not three: a third string's second beat costs CPU the benchmark doesn't have. Age 0 is bit-identical to the calibrated piano. |
| Stereo Width below 1 crossfades from one coherent microphone (the voices without mic phase or key pan, and the left microphone's view of the board) | `PianoEngine.cpp`, `Soundboard.cpp` | Analytic | Mono bounces: summing the spaced pair notches partials by more than 6 dB; at width 0 no partial of C3 notches by even 1 dB at any Mic Position (`engine_tests`). |
| Hammer Hardness h scales Q0 by h^p; Dynamic Range also scales the knock's distance from mf | `PianoEngine.cpp` | Analytic | Control mappings. A linear Q0 scale moved C4's treble share by only ~1 dB over the whole range, because the string's give sets mid-register contact times. |
| Dampers in two stages: the release poles (fundamental T60 0.12-0.75 s, bass slowest; partial k's shorter by 1 + 0.007k + 0.0003k^2) until the fundamental has fallen 8 dB (25 dB at E6, the top damper), then over 25 ms a residual 3 s / release T60 times slower, and at least 2.5 times faster than the free string | `InstrumentModel.cpp`, `PianoEngine.cpp` | Selected | VCSL's Knight upright releases (CC0, `model/vcsl-provenance.json`), broadband, re the held note, A2 / A3 / A4: drop in the first 100 ms -12.5 / -11.8 / -9.8 dB, the residual over 150-500 ms -17.1 / -17.7 / -17.3 dB, fading at 20 / 20 / 9 dB/s; its undamped strings fall at 6-11 dB/s. Now -11.6 / -13.1 / -13.0, -18.9 / -18.7 / -17.4 and 25 / 16 / 20. The single fast stage left -40 to -80 dB residuals; Pianoteq, bounced in our host for diagnosis only, leaves -25 to -35. Catching a released note by re-pedalling 50 ms later now gains 4 dB, not 6. |
| Key-release thump: each key let go kicks one envelope (3 ms rise, 30 ms fall, a difference of exponentials), on noise low-passed at 500 Hz into the board, 42 dB under a mf C4's peak; half with the pedal holding the damper off; scaled by Hammer Noise | `PianoEngine.cpp` | Selected | VCSL's Yamaha upright release samples (CC0): unpitched (their energy isn't at the note's harmonics), 100 Hz-1 kHz with the case's peaks near 190 and 330 Hz at every key, peaking ~8 ms in and 20 dB down 50-80 ms later. A vl2 release peaks 37 / 42 / 32 dB under its vl2 note at C2 / C4 / C6; the layers' levels don't follow velocity, so this doesn't. Test: `releasesThump`. |

The damper felt's own noise on release, beyond the thump, is not modelled.

## Fitted spectrum and decays

Added in November 2026 after a per-partial review: the octave-band metrics above averaged away errors of
22-24 dB in individual partials' levels. `tools/fit_piano.py` measures each partial's level 0.1 s after the strike
(both channels' power, a line over 0.05-0.4 s) in the recordings and in renders, and fits the tables below. Fits use
layers 1, 3 and 4 and the keys outside the every-third-key split; layer 2 and the split keys are the check.

| Table | Code | Status | Evidence |
|---|---|---|---|
| A unison's slow mode at its fitted level: where the coupled pair can't radiate it, the slow mode's excitation is raised to it (the strings' other polarization, along the board), never above the fast mode's; it has no strike coupling | `InstrumentModel.cpp` | Fitted (the level), Analytic (the mechanism) | Two coupled strings can't radiate a strong slow mode beside a much faster one: their antisymmetric motion cancels at the bridge. C4's 3rd partial (fast 2 s, slow 16 s) reached -18 dB at any beat rate; the fit asks -11, and C4 / C3's 2nd, 3rd and 6th partials fell 4-10 dB short, so held notes died early (C4 at 2 s 7 dB under Meta, 250 Hz-1 kHz). Now C3 / C4 at 2 s: -18 / -26 dB, Meta -15 / -25 (were -24 / -32). Knee error L1-L4 6.5 / 5.3 / 5.0 / 5.1 to 5.0 / 4.1 / 3.8 / 4.4 dB after refitting decays and spectrum. Fitting the decays from each partial's peak with log-time weights was tried and dropped: it chose faster first stages, and chords peaked 7 dB hotter. |
| Each partial's frequency, cents on the calibrated series (`fit_piano.py tune`) | `PartialTuning.hpp` | Fitted | The calibration measured the bass's partial frequencies to about the 16th and filled the rest from the stiff-string law, which the wound strings don't follow: D1's partials run -20 / -77 / -148 / -214 cents flat of it at the 20th / 30th / 40th / 50th (the same in the 1-3 s and 2-4 s spectra, where phantoms have died). The model played them there, and the fits measured the recordings off-peak, 3-6 dB weak. Followed up each key's late spectrum; a key moves only if 15 partials stand 6 dB stronger at their tracked frequencies than at the model's (a unison's doublets make the track hop between strings from C2 up) and its 30th and 40th sit within 45 cents of its moved neighbours. 14 keys move: A#0-D1, F1-A#1, D#2, E2, G2. |
| Strike position per key, replacing 0.115 + 0.00045 (n - 60) | `SpectrumCalibration.hpp` | Fitted | The recordings' partials show the strike's comb: B1's partials 8, 16 and 24 sit 25-35 dB under their neighbours, a strike at 1/8, where the analytic law put it near 1/10 and left those partials 20-30 dB loud. Each key's levels, detrended by a running median over 5 partials, against the comb averaged over strikes spread +-0.3% of the string (a unison's strings and the felt's contact don't meet at a point, so the notches fill in up the series): A0-B3 fit 1/8.0-1/8.5 (35-89% of the detrended variance), the treble 1/9-1/10 where its few partials identify it; the others take their neighbours'. |
| Radiation, dB over (key, 1/12 octave) | same | Fitted | A smooth surface (knots every 6 keys and 1/6 octave, second-difference smoothing) through layer 3's level differences less their median, each partial weighted by the square root of its amplitude so the fit follows the partials that carry the sound. It holds what the board and microphones make of each frequency: mid fundamentals were 12-16 dB strong, treble fundamentals 13 dB weak, the bass's 1-1.5 kHz band missing. Applied to each mode's radiated weight at load, not to the bridge force the board and sympathetic strings feel; each key's own cuts (below, not its lifts) reach that force too: the string has less of the partial. Uncut, the board rang what the fit had taken out of the direct sound (D1's 49th partial, -40 dB there, as loud as its neighbours through the board). |
| Each key's own partials, dB (partials 1-112) | same | Fitted | What layer 3's differences leave after the surface, in full where the recording holds the partial 10 dB over its floor (which biases it by 0.4 dB), fading to none at 6 dB, within +-40 dB (was 20 / 10 dB and +-25: the low bass's partials above 2 kHz, 14-19 dB over the floor and needing ~30 dB over the smooth surface, stayed 7-14 dB weak at every velocity). A smooth law reproduces the median partial but not the recordings' +-8 dB scatter about it, whose energy runs ~7 dB higher; ff was 5 dB dark for that reason.. Then each partial is evened over 5 keys, a running median and then a running mean: a regulated piano. Taken note by note Meta's is uneven (E5's octave 7-10 dB over any neighbour's at every velocity, D#5 5 dB loud), and E5 and F5 sounded distinctly different. Neighbouring keys' residuals at the same frequency scatter -40 to +13 dB (E5's octave isn't the room or the board), so it is the strings' and hammers' voicing. The median alone kept the edge between registers as a step, C#5-E5's octaves 17 dB over F5-G5's, between E5 and F5; the mean ramps it. Age adds unevenness on purpose. It costs each note's fit to its own recording: partial level error L1-L4 4.7 / 4.0 / 0.9 / 3.3 to 5.0 / 4.8 / 4.1 / 5.0 dB, spectrum error 4.05 to 4.82 dB. |
| Velocity, dB at velocity 40 and 120 re 80, over (key, 1/6 octave) | same | Fitted | Smooth surfaces (knots every 12 keys), uniform weights, through each partial's difference at layers 1 and 4 less its difference at layer 3. Applied per voice to what the hammer puts into each mode (an excitation filter), linear in log velocity, so a restruck string's ringing keeps its spectrum. The hammer reads each mode through the inverse share, so it meets the physical string: read through the shares, a boosted mode made ff unstable at very low sample rates. A partial the recording doesn't hold over its floor pulls down to that floor. The soft law reaches +30 dB/octave at 5-9 kHz, where the recordings at velocity 40 are that much brighter than the soft hammer. Each strike's shares are held to at most +30 dB, the most the recordings ask. Extrapolated to velocity 20, or scaled by Velocity Brightness 2 or Hammer Hardness 0.25, they reached +117 dB and peaked a treble note at +39 dBFS. Below velocity 40 they also made C2 louder at velocity 13 than at 40. |
| Brightening, dB at 40 and 120 re 80 and re the fundamental | same | Fitted | The recordings' own brightening, the same surfaces through each partial's level change from layer 3. Velocity Brightness scales it (0: every velocity with mf's spectrum); Hammer Hardness h moves along it by log2(h) octaves of velocity at the same level, as the felt's stiffness changes too. Hardness alone moved C4's treble share ~1 dB once the click below was gone. |
| Prompt and aftersound decay factors and the slow mode's level per partial | same | Fitted | Each partial's envelope at layer 3, fitted directly as A e^-2at + B e^-2bt over its floor, 0.03-5 s, data and curve both power-averaged over 0.3 s (through the unison's beats): a calibrated partial is a coupled unison (a its prompt decay, b its aftersound, B/A its slow mode's level), an extended partial one exponential. Fits that rise 10 dB over the floor and stay within 4 dB rms count (1384 prompt, 967 aftersound on fitting keys; unaveraged 875 / 763); the others take a smooth surface through them. At 20 dB and 4x, the bass's upper partials, which ring on 15-20 dB over their floor for seconds, took the surface and died early: 3-9 kHz partials A0-B2 were 9 / 20 / 33 dB under the recordings at 0.4 / 0.8 / 1.6 s, now 0 / 2 / 5. Bounded to 1/8-16x the prompt law and 3x the aftersound law: within 5 s a slower, quieter aftersound is indistinguishable from a plateau of noise or other strings (C2 otherwise fitted 300 s). Fitting the early slope, late slope and knee instead left them matched but the shape wrong: G#6 fell at -160 dB/s then plateaued, where the recording eases from -60.. Unaveraged, beats' dips pulled a fit fast and its aftersound low (C6's fundamental -60 dB/s and -19 dB, where its beat-averaged level falls at -21 and holds at -16), or failed it (G#7's at 7.8 dB rms, so the top treble took the law, twice too fast). Then, as the partials, evened over 5 keys. |

Also corrected:

- **Restrike runaway (bug, since 0.1).** A restruck string's ringing is rescaled to the new strike's gain so it sounds on unchanged, up to 100x for a soft strike on a loud note. The hammer met that rescaled ringing, so a soft restrike just after a loud note's release peaked 36 dB over the loud note. Repeated restrikes grew without bound, to +266 dB in a Bitwig session. The per-velocity shares made it far more likely. The hammer now reads only the motion its own strike put into each mode, kept in parallel while it is on, so it lands as on a still string. A fresh strike is bit-identical. Test: `nothingRunsAway` in `tests/engine_tests.cpp`.
- **Phantom partials (bug).** Since `295fbb8` a fresh note's phantoms were scaled by its own gain (the restrike
  carry ran on clear()'s gain of 1), 20-30 dB down; the phantom metric read -10.5 dB. The engine test guards it.
  The aggregate metric hides a real gap: E1's recorded phantom f6 + f7 stands 10 dB over partial 13, the model's
  25 dB under, and other bass keys' phantoms are 5-10 dB strong. The longitudinal resonance at 13.06 f0 isn't
  placed per key.
- **A unison's image (bug).** The Spaced Pair's phase between the channels was hashed per mode, so a partial's two
  coupled modes sat at different phases: C6's slow mode at 160 degrees, its prompt at 132, so in mono the aftersound fell
  7 dB further than the prompt, and in stereo the note moved as it decayed. Hashed per partial now. Test:
  `unisonsShareTheirImage`.
- **Evaluator.** Brightness rendered 0.6 s, so the renderer released the key at 0.24 s, inside the metric's
  10-400 ms window, while the recordings' keys stay held; it now renders 1 s. The old model rescored: L1-L4
  -0.4 / +2.4 / +1.6 / -0.8 dB.

Not fitted, and what remains:

- **The attack, after the board build-up.** Aligned on the peak, a note's first 4 ms over its 0.1-0.5 s level are
  now +0.6 / +0.5 / +0.8 dB against Meta's at velocity 40 / 60 / 120 (14 keys; were +2.5 / +2.2 / +2.3), C6 +4 / +3 at
  40 / 120 (were +8 / +6). Its fundamental's decay was the beat-skewed fit (see the decay row). Left: the top octave's
  first 10 ms at velocity 120 stand 7.6 dB over its body against Meta's, as before, where its knock keeps its level, and
  the attack's 2-8 kHz share reads 1-1.5 dB brighter than before the build-up (L1-L4 +1.7 / +4.2 / +5.1 / +2.8 dB):
  the board builds a partial below 3 kHz over 10 ms, above it faster.
- **The stereo image.** The Spaced Pair's phase now spreads from none at 100 Hz to fully diffuse at 1 kHz (over the
  A/B project +0.97 / +0.82 / +0.25 L/R correlation at 60-200 / 200-500 / 500-1000 Hz, from +0.19 / -0.32 / -0.37), so
  no partial below ~550 Hz can cancel in mono. The evaluator's per-note correlation is now +0.47 against the recordings'
  -0.01: a deliberate departure, as a random fixed phase per partial left whole bands anti-phase.

- **The forte bass's energy between its partials.** It is phantom partials: Meta's peaks there sit within 0-3 Hz of
  sums of two partials. With every pair driven (the phantom row), A#0-A#1's energy between partials (0.4-4 kHz,
  0.03-0.33 s) is -23 / -14 dB re the partials at velocity 40 / 120, Meta's -22 / -13 (was -31 / -30). The 150-700 Hz
  phantoms now read 3.4 dB strong on average (were +0.1), on keys that scatter: E1's 10 dB weak, others 5-10 strong.

- **Beating.** The unisons' beat rates stay analytic. Whole-note sustain error, sampled at a few instants, is
  dominated by where each strong partial sits in its beat: below F#6 it is 8.1 dB, as before (7.8).
- **The treble's tail.** From G#6 up the recordings' tails after ~1 s are room, board and the undamped treble
  strings' sympathy, as loud as the note (G#7 at 1 s: below-f0 content -29 dB, its fundamental -49 dB). The
  sustain metric renders the model without sympathy, so the top keys score 18-26 dB.
- **The top octave's first half-second** carries ~5 dB more energy than the recordings' (G#7, C8), though their
  fundamentals' levels match.
- **CPU:** partials to 9 kHz add ~40%: 64 voices under the pedal take p50 559 us of a 1333 us block (was 390). With the tuning (a few more partials fit under 9 kHz), the sheen and the release thump: p50 566 us, p99.9 0.84 of the block, max 1.10. The board build-up keeps each strike's own motion running up to ~90 ms: p50 620 us, p99.9 0.66-0.68 of the block, max 0.91-0.92 (repeat runs). Since the mode banks (`ModeBank.hpp`: each key's modes packed structure-of-arrays, a string the hammer has left run a block at a time with its lanes side by side, the hammer's contact per sample, the same arithmetic both ways): p50 277-282 us, p99 ~490 us, p99.9 0.60-0.61, max 0.98-1.00 (the 64-note chord's first block, in contact). Ringing 279 us a block where it was 623, the board's build-up 480-570 where it was 765-840, the hammer's contact as before (1200-1320). Output against the tree before: -190 dB over the A/B project.
- **The attack transient.** Above 4 kHz a note's first 4 ms stand 8-24 dB over its ringing 20-60 ms later, the
  recordings' 2-7 dB, in the original model too. With the radiation tables flat it is 12-20 dB, so they add up to
  ~15 dB on some keys (G4). Not the knock, board, phantoms, the felt's hysteresis or the force's smoothness: smoothing
  the force into the strings made it worse. Tried and removed: subtracting the static push of modes above 1.5 kHz
  (or 3 f0) from the radiated force. It assumes they follow the hammer statically, which holds only far above its
  1-4 ms contact; on G4, E4, C5 and G5 it over-subtracted into a plucked-string click (G4 at mf 19 dB under its peak,
  the recording 49 dB), which was heard. Subtracting only what the radiation weights add changed nothing.
- **Noise levels.** Pedal Noise is 20 dB under its first choice: the dampers landing 39 dB under a mf C4's peak,
  lifting 49 dB. The strings' output gain is 0.018 (was 0.04): the fitted tables had raised them ~7 dB, a single
  velocity-127 note to -4.9 dBFS; it peaks at -12 dBFS again.

| Metric | `fbeb14d` with the phantom fix | After the spectrum fit | Now: decays refit, tuning, trust at 10 dB |
|---|---|---|---|
| Partial level error, mean abs., L1 / L2 / L3 / L4 (fitting keys) | 23.8 / 22.0 / 23.1 / 23.1 dB | 6.5 / 5.8 / 3.1 / 5.3 dB | 4.7 / 4.0 / 0.9 / 3.3 dB, at the tuned frequencies |
| Same, split keys | 24.1 / 21.9 / 22.8 / 22.9 dB | 7.2 / 5.9 / 3.0 / 5.5 dB | 5.4 / 4.0 / 1.0 / 3.7 dB |
| Spectrum error, layers 1/3/4 / held-out layer 2 | 6.16 / 5.77 dB | 4.37 / 3.57 dB | 4.21 / 3.75 dB |
| Key level std, layer 3 | 3.56 dB | 1.39 dB | 1.37 dB |
| 1-8 kHz brightness L1 / L2 / L3 / L4 | -0.4 / +2.4 / +1.6 / -0.8 dB | +0.8 / +4.1 / +0.5 / +0.4 dB | +1.0 / +4.4 / +1.3 / +1.0 dB |
| Attack 2-8 kHz share L1 / L2 / L3 / L4 | -3.7 / +1.9 / +3.4 / +1.5 dB | +6.0 / +1.4 / +0.4 / -2.4 dB | +3.3 / +1.4 / +0.6 / -2.2 dB |
| Knock, between-partial 20-100 ms (counts room and noise; see the knock row) | +0.4 dB | -6.1 dB | -5.5 dB |
| Velocity span L1-L4 | +0.4 dB | -1.1 dB | -1.1 dB |
| Sustain error, layer 3, keys below F#6 / all | 7.8 / 8.97 dB | 8.1 / 12.2 dB (see above) | - / 12.1 dB |
| Attack above 4 kHz, first 6 ms, L1 / L3 / L4 (16 keys) | | -8.1 / -4.8 / -5.2 dB (just before the sheen) | -2.7 / +1.0 / -1.0 dB |
| A0-A1's partials 49-112 re their fundamental, L3 | | -7 to -12 dB (after the decays refit) | -0.2 to -3.4 dB |

The attack averages hide C2-C3 still bright in a soft note's first 10 ms and C5-C6 dark at ff.

Board build-up, evened tables, every phantom pair (`evaluate_model.py` and the scratch measures above, against the
commit before them, `c9016e6`):

| Metric | `c9016e6` | Now |
|---|---|---|
| Spectrum error, layers 1/3/4 / held-out layer 2 | 4.05 / 3.52 dB | 4.82 / 4.13 dB (the evening's cost) |
| Partial level error, L1 / L2 / L3 / L4 (fitting keys) | 4.7 / 4.0 / 0.9 / 3.3 dB | 5.0 / 4.8 / 4.1 / 5.0 dB (each note against its own recording) |
| Key level std, layer 3 | 1.22 dB | 1.94 dB (Meta's own unevenness counts against it) |
| Sustain error, layer 3 | 11.12 dB | 9.53 dB |
| First 4 ms over the 0.1-0.5 s level re Meta, velocity 40 / 60 / 120 (14 keys) | +2.5 / +2.2 / +2.3 dB | +0.6 / +0.5 / +0.8 dB |
| Attack 2-8 kHz share L1 / L2 / L3 / L4 | +0.9 / +3.0 / +3.7 / +1.5 dB | +1.7 / +4.2 / +5.1 / +2.8 dB |
| A#0-A#1 between partials, velocity 40 / 120 (Meta -22 / -13) | -31 / -30 dB | -23 / -14 dB |
| Phantom partials, A0-A1 | +0.1 dB | +3.4 dB |
| Velocity span L1-L4 | +0.5 dB | +1.4 dB |
| L/R correlation (recordings -0.01) | +0.45 | +0.65 (a unison's two modes now share their image) |
| E5 / F5 octave re fundamental, layer 3 (Meta -9 / -18) | -9 / -19 dB | -15 / -16 dB |


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

The table's spectrum and brightness rows used a gate on the model (bands below -90 dB were
skipped), so they compared the model with the recordings' hiss. After the first 40 ms the
recordings hold only their noise floor above 8 kHz, a steady ~-84 dBFS per band that is noise-like
(no partial peaks) from 0.1 s until the samples fade out. Soft notes' 4-8 kHz is mostly floor too.
The evaluator now gates on the reference instead: a band within 6 dB of the recording's floor (its
quietest 390 ms after the first 0.4 s) counts only the model's excess over that floor. Re-scored,
the current build reads:

| Metric (reference-floor gate) | Above table's tree | Current |
|---|---|---|
| Spectrum error, layers 1/3/4 | 6.53 dB | 6.16 dB |
| Spectrum error, layer 2 | 6.22 dB | 5.77 dB |
| 1-8 kHz brightness L1 / L2 / L3 / L4 | +5.2 / +6.0 / +1.9 / -0.4 dB | +0.1 / +2.7 / +1.9 / -0.4 dB | +1.0 / +4.4 / +1.3 / +1.0 dB |
| Attack 2-8 kHz share L1 / L2 / L3 / L4 | +17.6 / +13.6 / +8.2 / +3.9 dB | -3.7 / +1.9 / +3.4 / +1.5 dB | +3.3 / +1.4 / +0.6 / -2.2 dB |
| Velocity span L1-L4 | -0.8 dB | +0.4 dB | -1.1 dB |
| Onset overshoot mean / max | -0.7 / +4.5 dB | -1.7 / +1.6 dB |

So the high end was not missing: soft notes were too bright, in two ways. The velocity shelf cut
too little below mf, and the knock skipped it, so a pp note began with an ff note's click. The
knock was also too bright in the bass at every velocity. The
attack metric (2 ms before to 10 ms after onset, re the attack's own energy) found the click: the
others start 10 ms in. In the recordings ff carries 13 / 19 / 13 / 20 dB more of its 0-400 ms
energy in 2-8 kHz than pp at C2 / C3 / C4 / C5. The model carried 5 / 0 / 1 / 6 dB more and now
carried 25 / 20 / 21 / 18 dB more before the knock's low-pass. Key level std and sustain error moved
by 0.01 and 0.05 dB; the metrics not in this table are otherwise unchanged.

Tried and not kept: a hammer contact patch of fixed length (Gaussian, 1.5-4 mm) instead of the
per-partial-number roll-off, partials extended to 8-10 kHz, and the bridge corner re-selected
(300-500 Hz). The best, 2.5 mm with 8 kHz and 380 Hz, gave the bass its missing 4-8 kHz partials
(-7.9 -> +1.9 dB after 100 ms) and layers 1/3/4 6.53 -> 6.07 dB, but held-out layer 2 stayed at
6.21 dB, key levels, sustain and velocity span got slightly worse, and C4's ff-over-pp brightness
fell below the engine test's 3 dB.

The largest remaining errors are four. The top octave plays about 9 dB loud. G#7's and C5's
sustain is off. Bass and mid notes are missing energy above 4-8 kHz. The bass's strings are too
bright at 2-4 kHz, F#1-F#2 by 7-14 dB through the whole note (C2 +14 dB), while above 4 kHz,
where the partial series stops, they are short. C2's attack is still 7-13 dB bright for that
reason, not the knock. The recordings' own C2 is 20 dB darker at 2-4 kHz than their C3; the model
has nothing that changes between the two (one string length below D3, no wound-string model), so
this wants a second piano's evidence before it is fitted.
