# Physical piano modeling for PianoCraft: primary-literature review and staged plan

## Scope and evidence conventions

This report applies published acoustic and audio-synthesis research to an **original, real-time C++ upright-piano CLAP instrument** calibrated from the CC0 Meta Piano corpus. It does not describe, infer, or attempt to match any proprietary piano implementation. It follows [`docs/clean-room.md`](clean-room.md): recordings are analysis inputs, not plugin playback material; only derived synthesis coefficients belong in the instrument.

Two kinds of statement are deliberately separated below:

- **Established result** means a claim supported by the linked primary paper, thesis, official standard, or official corpus record. Section and equation numbers are given where useful.
- **Project recommendation** means an engineering choice for PianoCraft. These choices use published model structures but are not claims that a paper validated this particular upright, corpus, or implementation.

The most useful primary sources for the present architecture are Bank, Zambon, and Fontana's real-time modal piano model; Bensa et al.'s measured stiff-string model; Rauhala and colleagues' excitation and inharmonicity estimation work; Bank's measured body-filter design; and Lehtonen and colleagues' pedal/damper measurements. Full bibliographic records and open-text links are collected at the end.

## Executive recommendation

**Keep the modal/resonator-bank engine. Do not rewrite it as a finite-difference or digital-waveguide model.** The immediate opportunity is not a more elaborate physical model; it is better identification of the modes already being computed.

Implement in this order:

1. **Measurement discipline and per-partial calibration:** robust frequencies, independent decay rates, and observed-envelope validation on held-out keys and the held-out velocity layer.
2. **Effective unison/two-stage decay:** a small number of secondary modes fitted only where the recordings support two components.
3. **Velocity-dependent excitation:** retain bidirectional nonlinear hammer contact, but fit a small, regularized set of excitation/brightness parameters across layers rather than many independent mechanical constants.
4. **Release, then body/radiation:** simple measured or manually bounded per-mode release first; an effective regional soundboard filter second, fitted only after freezing string and hammer parameters.
5. **Selective sympathetic resonance:** sparse, undamped-string response at a conservative level. Add duplex-string behavior only if a repeatable residual and listening test justify it.
6. **Optimize only after quality gates pass:** prune inaudible modes and benchmark. Consider commuted synthesis, longitudinal/phantom partials, hammer hysteresis, a physical damper-contact model, or finite-element/finite-difference soundboards only for a demonstrated remaining defect.

This ordering follows the demonstrated feasibility of a complete real-time modal piano and its independently parameterizable partials, secondary resonators, and reduced soundboard representations ([Bank, Zambon & Fontana, 2010, §§III–VIII](https://home.mit.bme.hu/~bank/publist/taslp10.pdf)). It also reflects the fact that the current project already has that architecture, while its calibration report leaves excitation, release, body, sympathy, and stereo behavior analytic and flags substantial decay-estimator uncertainty ([`model/calibration-report.json`](../model/calibration-report.json), `limitations` and `quality_summary`).

## 1. What the corpus can and cannot identify

The official Meta Piano inventory contains 352 stereo 24-bit/48 kHz WAV files: all 88 keys from A0 to C8, four velocity layers, one variation per key and layer. The collection is CC0-1.0 ([official Meta Open Sound README](https://github.com/facebookincubator/Meta-open-sound/blob/main/README.md); [official manifest](https://raw.githubusercontent.com/facebookincubator/Meta-open-sound/main/systems/instruments/meta-piano/manifest.json); [collection license](https://github.com/facebookincubator/Meta-open-sound/blob/main/systems/instruments/meta-piano/LICENSE)). These are unusually useful calibration data because pitch, inharmonic partial frequencies, decay, dynamics, and stereo level can be measured without redistributing audio in the plugin.

**Project recommendation:** treat the following as identifiable, with quality flags:

- per-key and per-partial frequency, amplitude, and decay;
- prompt versus late decay and closely spaced unison components where separable;
- excitation spectrum and loudness as functions of velocity layer;
- relative stereo level and shared left/right output coefficients;
- an *effective* radiation/recording-chain response shared across notes.

Treat these as **not directly identified by the documented corpus**:

- a bridge-force-to-pressure impulse response, which requires measured input force and output response; the published soundboard and sustain-pedal calibration methods use precisely such measurements ([Bank, 2007, §§1–4](https://home.mit.bme.hu/~bank/publist/icmc07.pdf); [Zambon, Lehtonen & Bank, 2008, §3](https://dafx.de/paper-archive/2008/papers/dafx08_37.pdf));
- hammer mass, stiffness, exponent, strike position, and string mass as separate physical constants: radiated audio alone does not uniquely separate their joint effects;
- controlled damper distance, part-pedaling, pedal-down string-register response, or damper-contact force: the public inventory documents notes and velocities, not those experimental conditions;
- physical soundboard geometry, wood properties, rib spacing, or individual board modes: the microphone recordings also contain strings, radiation, microphone response, and room effects;
- polyphonic behavior, restrikes, or sympathy between simultaneously played notes, because the manifest documents isolated notes and one take per condition.

A single recording per key/layer also means there is no repeated-take estimate of measurement variance. Cross-velocity disagreement must therefore remain visible per key rather than being hidden by a single median score.

## 2. Model structures that matter

### 2.1 Stiff strings as independently measured modes

**Established result.** For a hinged, stiff string, modal synthesis gives one decaying oscillator per transverse mode. Bank et al. write the modal impulse response as an exponentially damped sinusoid and implement it with a two-pole resonator; independent modal loss coefficients allow frequency-dependent decay. Their approximate partial-frequency law and hammer-position weights are

\[
 f_k \simeq k f_0\sqrt{1+B k^2},\qquad
 w_{\mathrm{strike},k}=\sin(k\pi x_h/L),
\]

with fundamental \(f_0\), inharmonicity \(B\), and strike position \(x_h/L\) ([Bank, Zambon & Fontana, 2010, Eqs. 3–16](https://home.mit.bme.hu/~bank/publist/taslp10.pdf)). Rauhala et al. give the equivalent form using the *measured first partial*, eliminating the theoretical \(f_0\):

\[
 f_k=k f_1\sqrt{\frac{1+B k^2}{1+B}}.
\]

Their estimator also warns that real tones contain non-transverse peaks and multiple slightly detuned strings ([Rauhala, Lehtonen & Välimäki, 2007, §§1–2, Eq. 1](https://aaltodoc.aalto.fi/bitstreams/5c91f36d-c35a-43a7-9701-49b61eaf260c/download)). Bensa et al. independently estimate frequency-dependent loss and stiffness from bridge measurements and describe the resulting parameters as an *equivalent* string because several physical effects are folded into them ([Bensa et al., 2003, §VI and Conclusions](https://www.pure.ed.ac.uk/ws/files/11222473/1.1587146.pdf)).

**Project recommendation.** Retain the existing second-order mode update. For each accepted mode, store \(f_k\), amplitude-decay coefficient \(\alpha_k\), excitation/input coefficient, and left/right output coefficients. With \(T_{60,k}\) measured from amplitude,

\[
 \alpha_k=\frac{\ln 1000}{T_{60,k}},\qquad
 p_k=e^{(-\alpha_k+j2\pi f_k)/f_s},\qquad
 T_{60,k}=\frac{\ln 1000}{\alpha_k}.
\]

Thus \(r_k=|p_k|\), and the recursive denominator uses \(2r_k\cos(2\pi f_k/f_s)\) and \(r_k^2\). This is the same pole structure used in the current engine and in the modal literature, not a new physical claim.

Fit a single \(B\) only as a compact prior where it explains several strong partials. Otherwise use directly measured \(f_k\) or smooth residuals in cents; do not force high-treble notes into a single-string model merely to avoid a fallback. Call fitted frequencies and decay values **effective modal parameters**, not direct measurements of string geometry.

### 2.2 Unison, beating, and prompt/aftersound decay

**Established result.** Slightly mistuned strings and polarization/bridge coupling produce beating and a faster early followed by a slower late decay. Bank et al. implement these effects with secondary resonators and note that a reduced set can target only prominent partials ([Bank, Zambon & Fontana, 2010, §VI-A](https://home.mit.bme.hu/~bank/publist/taslp10.pdf)). Weinreich's earlier coupled-string analysis is the foundational physical reference ([Weinreich, 1977, DOI](https://doi.org/10.1121/1.381677)); the implementation-specific conclusion here relies on the fully reviewed Bank et al. text. Bensa et al. show that the same measurements can calibrate an equivalent string or coupled waveguides, so the choice of resonator topology does not change the identification goal ([Bensa et al., 2003, §§VI–VII](https://www.pure.ed.ac.uk/ws/files/11222473/1.1587146.pdf)).

**Project recommendation.** Replace analytic secondary detuning and aftersound multipliers with an *effective two-component model* only where supported:

\[
 x_k(t)=A_{1,k}e^{-\alpha_{1,k}t}\sin(2\pi f_{1,k}t+\phi_{1,k})
       +A_{2,k}e^{-\alpha_{2,k}t}\sin(2\pi f_{2,k}t+\phi_{2,k}).
\]

Start with a handful of the strongest low and mid partials per note, reusing the existing secondary bank. Estimate early and late log-envelope slopes, then fix candidate poles and solve component amplitudes by linear least squares. Accept the second component only if it improves a held-out portion of the envelope and is consistent across fitting layers. Otherwise keep one mode. Do not interpret the two components as uniquely identified physical strings; they are a compact effective description.

This addresses a limitation of static detuning alone: it can produce beats but does not by itself identify the measured early/late energy redistribution.

### 2.3 Hammer-string contact and velocity

**Established result.** A standard struck-string model uses a hammer mass and a one-sided nonlinear compression law,

\[
 F_h=K_h\max(y_h-y_s,0)^p,\qquad m_h\ddot y_h=-F_h,
\]

so contact duration and repeated interaction emerge from hammer and string motion rather than a fixed pulse length ([Chaigne & Askenfelt, 1994, §I, Eqs. 4–6](https://people.cs.uchicago.edu/~ridg/stabil/pianostring.pdf)). Bank et al. use the same model with modal strings and explain how an initial delay in the modal impulse response avoids a delay-free feedback loop ([Bank, Zambon & Fontana, 2010, §§III-B–IV](https://home.mit.bme.hu/~bank/publist/taslp10.pdf)). A more signal-oriented alternative fits partial amplitudes and a velocity-dependent equalizing filter from recordings; Rauhala and Välimäki show louder/brightness-changing behavior and deliberately make the excitation parameters easy to obtain from recorded tones ([Rauhala & Välimäki, 2006, §2](https://aaltodoc.aalto.fi/server/api/core/bitstreams/f4a15f49-bbc5-4fec-a13b-87402aabf06a/content)).

**Project recommendation.** Keep the bidirectional contact model because it naturally supports restrikes and existing string state. Fit only a reduced set first: one excitation level law plus shared/register-smooth stiffness or brightness parameters. Use layers 1, 3, and 4 for fitting and keep layer 2 untouched. Optimize early-partial log amplitudes and a brightness measure across layers, with a level term so brightness cannot be achieved merely by changing gain.

Do not independently fit \(m_h,K_h,p,x_h\), string mass, and force limits from these recordings. Their effects are coupled, and the corpus has no hammer motion or contact-force channel. If the reduced physical fit is unstable across held-out keys, use Rauhala and Välimäki's velocity-dependent excitation-filter idea as the fallback *excitation law*, while retaining the measured modal string bank. Add felt hysteresis only after a specific attack defect survives this simpler calibration.

### 2.4 Soundboard and radiation as an effective filter

**Established result.** In the reduced models, body termination effects are folded into string frequency/decay parameters, while radiation is represented by a filter. Bank designs parallel second-order body filters by fixing poles and solving feedforward coefficients by least squares; a parallel FIR path improves the onset for a far-miked, non-minimum-phase response ([Bank, 2007, §§3–4](https://home.mit.bme.hu/~bank/publist/icmc07.pdf)). Bank et al. compare this structure with partitioned convolution and explicitly note that logarithmically placed poles need not be the actual soundboard modal frequencies ([Bank, Zambon & Fontana, 2010, §VII](https://home.mit.bme.hu/~bank/publist/taslp10.pdf)). Measurements on an upright soundboard found essentially linear response in the tested regime and different behavior below and above roughly 1 kHz, with spatially localized modes at higher frequencies ([Ege, Boutillon & Rébillat, 2013, abstract and conclusions](https://arxiv.org/abs/1212.2323)). Reduced subplate/inter-rib models reproduce position-dependent mobility trends, supporting regional rather than purely global radiation models ([Boutillon & Ege, 2013, abstract and §§1–3](https://arxiv.org/abs/1305.3057)).

**Project recommendation.** Keep a direct/short-FIR path plus a small parallel resonator bank per region:

\[
 H_r(z)=D_r(z)+\sum_j\frac{d_{r,j,0}+d_{r,j,1}z^{-1}}
 {1+a_{r,j,1}z^{-1}+a_{r,j,2}z^{-2}}.
\]

This is an effective bridge/radiation/microphone/room filter, not a claim to recover physical soundboard modes. Preserve the direct path for attack fidelity; fit shared poles across regions and separate regional and left/right residues where the data support them. Do not simultaneously refit string damping and body damping.

Because no bridge impulse response is available, freeze strings and excitation first, then fit only reproducible residual features shared across notes. Use short-time residual behavior and smoothed spectral shape, regularize strongly across keys, and reject fits that merely chase one note's noise or the late room tail. If future licensed measurements include hammer force and radiated pressure, Bank's fixed-pole least-squares design or Zambon et al.'s frequency-zoomed ARMA plus least-squares procedure becomes directly applicable ([Zambon, Lehtonen & Bank, 2008, §3](https://dafx.de/paper-archive/2008/papers/dafx08_37.pdf)).

### 2.5 Damper release and part-pedaling

**Established result.** Bank et al.'s real-time model shortens resonator decay at note-off and explicitly leaves a physical damper model for future work ([Bank, Zambon & Fontana, 2010, §IX](https://home.mit.bme.hu/~bank/publist/taslp10.pdf)). Measurements of part-pedaling show three regimes: initial free vibration, rapid damper-string interaction, then a slower final free decay; nonlinear amplitude limitation can alter timbre during contact ([Lehtonen, Askenfelt & Välimäki, 2009, official abstract and companion page](http://research.spa.aalto.fi/publications/papers/jasael-part-pedaling/); [DOI](https://doi.org/10.1121/1.3162438)).

**Project recommendation.** For the first release milestone, use a smooth transition from sustain to a small set of per-mode release coefficients; avoid an output-gain choke that cuts all frequencies identically. Validate clicks, prompt decay, and residual tail by register. Keep part-pedaling and physical damper noise out of scope until there are controlled note-off recordings. Do not claim that an undocumented sample tail identifies damper-contact physics. A later controlled corpus should include note-off timestamps and several damper distances; only then fit a three-stage release or contact model.

### 2.6 Sympathetic strings and duplex scale

**Established result.** Sustain-pedal measurements show increased sympathetic content and register-dependent changes to partial decay and two-stage envelopes; one demonstrated implementation uses a reduced set of string models rather than a generic room reverberator ([Lehtonen et al., 2007, official abstract and companion page](http://research.spa.aalto.fi/publications/papers/jasa-piano-pedal/); [DOI](https://doi.org/10.1121/1.2756172)). Zambon et al. calibrate a parallel second-order string-register model from soundboard/open-string impulse responses and use a small gain matrix between keyboard regions ([Zambon, Lehtonen & Bank, 2008, §§2–4](https://dafx.de/paper-archive/2008/papers/dafx08_37.pdf)). In grand pianos, Öberg and Askenfelt measured audible front and rear duplex contributions; their ABX experiment found complete front-duplex damping clearly perceptible and rear-duplex effects less pronounced ([Öberg & Askenfelt, 2012, PubMed abstract](https://pubmed.ncbi.nlm.nih.gov/22280708/); [DOI](https://doi.org/10.1121/1.3664049)). That study is evidence not to dismiss duplex behavior, not evidence for a particular upright implementation.

**Project recommendation.** Drive only undamped candidate modes from the dry bridge signal and gate their decay with damper state. Emphasize near-coincident partials, with sparse coupling gains and a conservative global level. Do not use a broadband reverb-like wash as a substitute. Since the corpus has no documented pedal conditions, fit no pedal-coupling matrix from it; tune one or two levels by listening and document them as analytic. Add front/rear duplex modes only after residual analysis finds repeatable non-main-string peaks and an AB comparison shows a benefit without excessive brightness.

## 3. Calibration procedure for Meta Piano

The following is a **project-designed estimation pipeline** based on the measured-mode approaches above, not a claim to reproduce any paper's full algorithm.

### 3.1 Preserve a real test set

1. Keep the existing every-third-key held-out pattern and all of velocity layer 2 out of fitting, interpolation, smoothing, threshold selection, and model-order selection. Layers 1, 3, and 4 may estimate shared parameters or a velocity law; layer 2 is evaluated only after choices are frozen.
2. Reserve some fitting keys as a development set while tuning estimators. Once the final held-out split has guided several design changes, it has become development data and should be described as such.
3. Save source checksum, estimator version, accepted/rejected partials, fit windows, SNR, observed dB drop, residual error, and fallback reason in provenance. The current archive checksum and CC0 record are already tracked in [`model/provenance.json`](../model/provenance.json).

### 3.2 Estimate strings before excitation or body

For each key and fitting layer:

1. Detect strong partials, reject likely non-transverse peaks, and estimate \(f_1,B\) from the stiff-string law where its residuals are small. This follows the partial-frequency-deviation concept and its quality diagnostics ([Rauhala, Lehtonen & Välimäki, 2007, §2](https://aaltodoc.aalto.fi/bitstreams/5c91f36d-c35a-43a7-9701-49b61eaf260c/download)).
2. Independently refine \(f_k\) for high-SNR modes. If the one-\(B\) law is poor, retain those frequencies directly and interpolate only missing/uncertain modes. Smooth log-frequency and cents residuals across reliable neighboring keys, not across held-out keys.
3. Isolate each mode with a zero-phase bandpass/heterodyne and estimate the analytic envelope. Fit early and late windows separately; use the observed log-envelope residual and SNR as acceptance criteria, following the general principle that modal decay follows from component amplitude modulation ([Bensa et al., 2003, §VI-B](https://www.pure.ed.ac.uk/ws/files/11222473/1.1587146.pdf)).
4. Fit the *rendered* envelope to the complete observed interval, not just a scalar \(T_{60}\). A 5–25 dB drop can estimate a slope, but a 30 s \(T_{60}\) from a much shorter recording is extrapolation, not a 30 s measurement. Mark clamped values invalid for acceptance rather than allowing them to look like successful fits.
5. Add a secondary unison component only under the identifiability gate in §2.2. Otherwise prefer one well-estimated mode to two arbitrary modes.
6. Fit shared free-decay parameters across fitting layers only if they are consistent; otherwise retain layer-specific estimates and investigate the cause. Do not average away a velocity-dependent recording condition.

Initial coefficient set per mode: \(f_k,\alpha_{1,k}\), optional \(f_{2,k},\alpha_{2,k}\), input amplitude(s), left/right output gains, and quality flags. Keep the current mode cap initially so calibration can be compared without a simultaneous performance rewrite.

### 3.3 Fit excitation second

- Normalize only within a layer for timbre fitting; retain a separate inter-layer loudness term for dynamics.
- Fit early-partial log amplitudes, high/low-band spectral ratio, and overall level across layers 1, 3, and 4.
- Prefer shared parameters by bass/middle/treble register and shrink per-key values toward them. Do not let every key acquire an independent hammer model.
- Validate attack click/onset, level error, and brightness on layer 2. Rauhala and Välimäki's measured velocity-filter structure is the fallback if a more physical parameterization is not identifiable ([Rauhala & Välimäki, 2006, §§2.1–2.3](https://aaltodoc.aalto.fi/server/api/core/bitstreams/f4a15f49-bbc5-4fec-a13b-87402aabf06a/content)).

### 3.4 Fit release, body, stereo, and sympathy last

- **Release:** if genuine note-off intervals are available and documented, fit per-partial release decay from them. Otherwise retain conservative register-smooth analytic values and mark them uncalibrated.
- **Body:** fit the smallest residual model that improves multiple held-out notes. Start with one shared response and regional gain/tilt before allowing all existing regional modes to vary independently. Use the direct/FIR path for early non-minimum-phase content as in Bank's body-filter examples ([Bank, 2007, §4.4](https://home.mit.bme.hu/~bank/publist/icmc07.pdf)).
- **Stereo:** fit relative sustained left/right gains or shared output residues after mono timbre is stable. Treat these as the recording perspective, not proof of physical key position.
- **Sympathy:** no numerical pedal calibration claim from this corpus. Validate selectivity and absence of a generic wash by ear, using the published sustain-pedal models as structural guidance ([Zambon, Lehtonen & Bank, 2008, §§2–4](https://dafx.de/paper-archive/2008/papers/dafx08_37.pdf)).

## 4. Staged implementation and acceptance gates

| Stage | Implement | Fit from CC0 recordings | Gate before proceeding |
|---|---|---|---|
| **0. Baseline** | Deterministic offline renders and a per-key/per-partial metrics table for all four layers. | Existing calibration plus raw-analysis features; no raw corpus playback in the plugin. | Reproducible coefficients/checksums; final held-out split documented; baseline listening notes recorded. |
| **1. Modal strings** | Direct per-mode frequency and independent decay calibration; honest extrapolation flags. | \(f_1,B\) or direct \(f_k\), early/late \(\alpha_k\), mode amplitudes. | Lower envelope and frequency error on untouched keys/layer without more clicks or unstable poles; no clamped fit counted as a pass. |
| **2. Effective unison** | Sparse secondary modes with accept/reject fallback. | Frequency separation, early/late decay, component amplitudes. | Better held-out envelopes and natural prompt/aftersound in level-matched listening; reject unsupported two-mode fits. |
| **3. Hammer/velocity** | Reduced nonlinear-contact and/or excitation-filter parameters. | Excitation level, early partial spectrum, brightness by layer. | Layer-2 dynamics become brighter as well as louder, without attack noise, clipping, or per-key discontinuities. |
| **4. Release** | Smooth per-mode sustain-to-release transition. | Only documented genuine note-off data; otherwise analytic bounds. | Prompt but unclipped release across registers; restrikes preserve state and remain click-free. |
| **5. Body/stereo** | Direct path plus small regional parallel-filter bank and shared L/R residues. | Reproducible residual spectral/time shape and relative stereo levels. | Improvement across several held-out notes, not one; no exaggerated room tail or noncausal/pre-onset artifacts. |
| **6. Sympathy/duplex** | Sparse undamped-string coupling; optional residual-driven duplex experiment. | No direct pedal fit from current corpus; residual peaks only for duplex candidates. | Selective pedal response and no broadband wash; disable any duplex addition that does not improve blind listening. |
| **7. Optimization** | Mode pruning, SIMD-friendly block work, polyphony cost management; optionally compare commuted excitation. | Only frozen coefficients/model-generated responses. | Existing engine tests, bit-identical block-size behavior where intended, real-time benchmark headroom, and no listening regression. |

Commuted piano synthesis is a legitimate later optimization, not the starting architecture. It relies on commuting linear time-invariant blocks and linearizing hammer behavior at a fixed collision velocity; its authors discuss the approximations for restrikes and nonlinear contact ([Smith & Van Duyne, 1995, especially “Commuted Piano Synthesis” and “Excitation Factoring”](https://ccrma.stanford.edu/~jos/pdf/svd95.pdf)). Use it only after calibration is stable, and generate any commuted response from the original model rather than embedding raw corpus playback.

## 5. Validation that can falsify the model

### 5.1 Objective checks

Report distributions and worst keys, not only medians:

- **Frequency:** absolute cents error per accepted partial and first-partial error; count of one-\(B\) fallbacks.
- **Sustain:** dB RMS envelope error over the actual observed interval; early and late slope/T60 relative error; observed dB drop and SNR beside each value.
- **Unison:** beat-frequency error where identifiable, early/late slope error, and secondary-model acceptance rate.
- **Velocity:** sustained-level error by layer, early log-partial-magnitude error, and high/low-band brightness error on held-out layer 2.
- **Release:** release-envelope error where ground truth exists, maximum click/discontinuity metric, and tail-level checks.
- **Body/stereo:** residual smoothed-magnitude error in several time windows, early residual error, and L/R level ratio; do not call this physical soundboard identification accuracy.
- **Robustness:** finite output at 44.1/48/96 kHz, sample-rate recomputation of poles, no coefficient discontinuities at note boundaries, block-size determinism, restrikes, pedal transitions, dense chords, and voice stealing.

These are project metrics derived from the quantities the modal literature actually estimates—independent frequency, damping, and amplitude—rather than a claim that one global spectral-distance score proves realism ([Bensa et al., 2003, §VI](https://www.pure.ed.ac.uk/ws/files/11222473/1.1587146.pdf); [Bank, Zambon & Fontana, 2010, §III](https://home.mit.bme.hu/~bank/publist/taslp10.pdf)). Preserve the current engine tests and add comparisons in the same form: coefficient generation, offline render, then actual CLAP-bundle behavior.

### 5.2 Listening checks

Use [`docs/listening-checklist.md`](listening-checklist.md) with dry, level-matched reference/render pairs for bass, middle, and treble at soft, medium, and loud layers. Randomize A/B order and record the listener, device, gain-matching method, defects, and accept/reject decision. Separately evaluate dynamics without level matching so the velocity law is not hidden.

For a small project, repeated pairwise comparisons against the corresponding recording are more actionable than a large listening panel. When comparing several candidate models, a formal MUSHRA design can be added: use an open and hidden reference, relevant hidden anchors, randomized presentation, trained listeners, and appropriate statistical reporting as specified by ITU-R BS.1534-3, rather than calling an informal multi-slider test “MUSHRA” ([ITU-R, 2015, §§3–9](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.1534-3-201510-I!!PDF-E.pdf)).

No objective metric or successful CLAP load is a perceptual pass. Bank et al. likewise leave the quality-versus-cost comparison of alternative body models to listening tests ([Bank, Zambon & Fontana, 2010, §IX](https://home.mit.bme.hu/~bank/publist/taslp10.pdf)).

## 6. Deliberately postponed work

- **Finite-difference/finite-element full piano:** global models can represent nonlinear strings, plates, and air in detail, but the wideband 3-D acoustic computation is explicitly high-cost and used high-performance parallel computing in Chabassier et al.'s work ([Chabassier, Chaigne & Joly, 2013, introduction and model](https://inria.hal.science/file/index/docid/787551/filename/RR-8181.pdf); [DOI](https://doi.org/10.1121/1.4809649)). It is a research reference model, not the next small-project CLAP milestone.
- **Digital waveguide conversion:** Bensa et al. show a credible path from a stiff lossy string to a measured waveguide, including coupled strings ([Bensa et al., 2003](https://www.pure.ed.ac.uk/ws/files/11222473/1.1587146.pdf)). That does not justify discarding an existing modal engine whose independent modes are already a good fit for calibration.
- **Longitudinal and phantom partials:** modal literature provides an alias-aware nonlinear implementation and reports greatest relevance for strong low-register playing ([Bank, Zambon & Fontana, 2010, §V](https://home.mit.bme.hu/~bank/publist/taslp10.pdf)). Add only if held-out residuals and listening isolate missing metallic/forced components after strings, hammer, and body are calibrated.
- **Hammer hysteresis and shank dynamics:** the simple power-law model is a deliberate reduced approximation; more elaborate contact physics adds poorly identified parameters ([Bank, Zambon & Fontana, 2010, §IV](https://home.mit.bme.hu/~bank/publist/taslp10.pdf)).
- **Physical damper contact/part-pedaling:** justified by measured phenomena, but not identifiable from the documented corpus ([Lehtonen, Askenfelt & Välimäki, 2009](http://research.spa.aalto.fi/publications/papers/jasael-part-pedaling/)).
- **Physical plate-derived soundboard:** upright-board modal research supports reduced and regional understanding, but the project has no geometry or mobility measurements with which to identify a physical plate ([Trévisan, Ege & Laulagnet, 2017, abstract and model](https://hal.science/hal-01456083/file/Trevisan_JASA2017_HAL.pdf); [DOI](https://doi.org/10.1121/1.4974860)).

## 7. Source verification limits

The core implementation recommendations above were checked against original full paper/thesis text. The following relevant sources could not be fully verified through accessible primary channels and therefore are either used only at abstract level or not used for technical recommendations:

- **Weinreich (1977), “Coupled piano strings”:** DOI metadata verified; no accessible full text was verified. Detailed unison implementation claims are instead cited to Bank et al. (2010).
- **Öberg and Askenfelt (2012), duplex stringing:** the official PubMed abstract was verified; the full paper was not accessed. Only abstract-level findings are stated.
- **Rauhala et al. (2008), “A Parametric Piano Synthesizer”:** DOI metadata verified, but the publisher full text was not accessed. It is not required for the recommendations.
- **Rigaud, David, and Daudet (2013), whole-compass inharmonicity/tuning:** publisher abstract and DOI were verified, but the full journal text was not accessed. No detailed algorithm from that paper is claimed.
- **Le Carrou et al. (2005), sympathetic-string modeling:** bibliographic metadata was found, but the full original text was not retrieved. No equations or parameter values from it are used.

No source consulted for this report supplies evidence that a particular analytic coefficient currently in PianoCraft is correct for the Meta upright. Such values remain hypotheses until calibrated and validated.

## 8. Primary bibliography

### Core string, hammer, modal, and calibration literature

1. **Balázs Bank, Stefano Zambon, and Federico Fontana (2010). “A Modal-Based Real-Time Piano Synthesizer.”** *IEEE Transactions on Audio, Speech, and Language Processing* 18(4), 809–821. [DOI: 10.1109/TASL.2010.2040524](https://doi.org/10.1109/TASL.2010.2040524). [Author-hosted full text](https://home.mit.bme.hu/~bank/publist/taslp10.pdf). Central for modal poles, nonlinear hammer contact, secondary resonators, sympathy, soundboard alternatives, and real-time structure.

2. **Julien Bensa, Stefan Bilbao, Richard Kronland-Martinet, and Julius O. Smith III (2003). “The Simulation of Piano String Vibration: From Physical Models to Finite Difference Schemes and Digital Waveguides.”** *Journal of the Acoustical Society of America* 114(2), 1095–1107. [DOI: 10.1121/1.1587146](https://doi.org/10.1121/1.1587146). [Institution-hosted published PDF](https://www.pure.ed.ac.uk/ws/files/11222473/1.1587146.pdf). Central for stiff/lossy strings and measured equivalent parameters.

3. **Antoine Chaigne and Anders Askenfelt (1994). “Numerical Simulations of Piano Strings. I. A Physical Model for a Struck String Using Finite Difference Methods.”** *Journal of the Acoustical Society of America* 95(2), 1112–1118. [DOI: 10.1121/1.408459](https://doi.org/10.1121/1.408459). [Original paper PDF, institutional mirror](https://people.cs.uchicago.edu/~ridg/stabil/pianostring.pdf). Primary hammer-contact model and numerical-stability context. Part II, **“Numerical Simulations of Piano Strings. II. Comparisons with Measurements and Systematic Exploration of Some Hammer-String Parameters,”** 95(3), 1631–1640, has [DOI: 10.1121/1.408549](https://doi.org/10.1121/1.408549); its full text was not used for claims here.

4. **Jukka Rauhala and Vesa Välimäki (2006). “Parametric Excitation Model for Waveguide Piano Synthesis.”** *Proceedings of ICASSP 2006*, pp. 157–160. [DOI: 10.1109/ICASSP.2006.1661236](https://doi.org/10.1109/ICASSP.2006.1661236). [Aalto-hosted original article in the author's dissertation collection](https://aaltodoc.aalto.fi/server/api/core/bitstreams/f4a15f49-bbc5-4fec-a13b-87402aabf06a/content). Practical measured excitation and velocity filtering.

5. **Jukka Rauhala, Heidi-Maria Lehtonen, and Vesa Välimäki (2007). “Fast Automatic Inharmonicity Estimation Algorithm.”** *Journal of the Acoustical Society of America* 121(5), EL184–EL189. [DOI: 10.1121/1.2719043](https://doi.org/10.1121/1.2719043). [Aalto-hosted published PDF](https://aaltodoc.aalto.fi/bitstreams/5c91f36d-c35a-43a7-9701-49b61eaf260c/download). Primary partial-frequency law, estimation cautions, and quality diagnostics.

6. **Balázs Bank (2007). “Direct Design of Parallel Second-Order Filters for Instrument Body Modeling.”** *Proceedings of the International Computer Music Conference 2007*, Vol. I, pp. 458–465. [Author-hosted full text and stable conference citation](https://home.mit.bme.hu/~bank/publist/icmc07.pdf). No DOI assigned in the paper record. Fixed-pole/least-squares body fitting and FIR-plus-resonator onset treatment.

7. **Julius O. Smith III and Scott A. Van Duyne (1995). “Commuted Piano Synthesis.”** *Proceedings of the 1995 International Computer Music Conference*, Banff. [Stanford CCRMA author-hosted full text](https://ccrma.stanford.edu/~jos/pdf/svd95.pdf). Stable primary conference URL; no DOI stated. Companion paper: **Scott A. Van Duyne and Julius O. Smith III (1995), “Developments for the Commuted Piano,”** [Stanford CCRMA full text](https://ccrma.stanford.edu/~jos/pdf/vds95.pdf), covering hammer, body, and coupled-string calibration.

8. **Gabriel Weinreich (1977). “Coupled Piano Strings.”** *Journal of the Acoustical Society of America* 62(6), 1474–1484. [DOI: 10.1121/1.381677](https://doi.org/10.1121/1.381677). Foundational physical background; full text was not verified for this report.

### Pedals, sympathy, duplex, and soundboard literature

9. **Heidi-Maria Lehtonen, Henri Penttinen, Jukka Rauhala, and Vesa Välimäki (2007). “Analysis and Modeling of Piano Sustain-Pedal Effects.”** *Journal of the Acoustical Society of America* 122(3), 1787–1797. [DOI: 10.1121/1.2756172](https://doi.org/10.1121/1.2756172). [Author/institution companion page](http://research.spa.aalto.fi/publications/papers/jasa-piano-pedal/); [open dissertation containing the original publication](https://aaltodoc.aalto.fi/server/api/core/bitstreams/73d23db2-7c82-4e91-a728-4cc961799397/content).

10. **Stefano Zambon, Heidi-Maria Lehtonen, and Balázs Bank (2008). “Simulation of Piano Sustain-Pedal Effect by Parallel Second-Order Filters.”** *Proceedings of DAFx-08*, pp. 199–204. [DAFx archive record](https://dafx.de/paper-archive/details/5mJ31JDQWZTHNiqjmf19mQ) and [open paper PDF](https://dafx.de/paper-archive/2008/papers/dafx08_37.pdf). No DOI stated. Direct calibration from measured body/open-string responses and regional coupling.

11. **Heidi-Maria Lehtonen, Anders Askenfelt, and Vesa Välimäki (2009). “Analysis of the Part-Pedaling Effect in the Piano.”** *Journal of the Acoustical Society of America Express Letters* 126(2), EL49–EL54. [DOI: 10.1121/1.3162438](https://doi.org/10.1121/1.3162438). [Author/institution abstract and sound examples](http://research.spa.aalto.fi/publications/papers/jasael-part-pedaling/); [open dissertation containing the original publication](https://aaltodoc.aalto.fi/server/api/core/bitstreams/73d23db2-7c82-4e91-a728-4cc961799397/content).

12. **Fredrik Öberg and Anders Askenfelt (2012). “Acoustical and Perceptual Influence of Duplex Stringing in Grand Pianos.”** *Journal of the Acoustical Society of America* 131(1), 856–871. [DOI: 10.1121/1.3664049](https://doi.org/10.1121/1.3664049). [Publisher-indexed open abstract](https://pubmed.ncbi.nlm.nih.gov/22280708/). Full text was not verified; only abstract-level results are used.

13. **Kerem Ege, Xavier Boutillon, and Marc Rébillat (2013). “Vibroacoustics of the Piano Soundboard: (Non)linearity and Modal Properties in the Low- and Mid-Frequency Ranges.”** *Journal of Sound and Vibration* 332(5), 1288–1305. [DOI: 10.1016/j.jsv.2012.10.012](https://doi.org/10.1016/j.jsv.2012.10.012). [Author-hosted preprint](https://arxiv.org/abs/1212.2323). Experimental linearity, damping, and frequency-regime evidence.

14. **Xavier Boutillon and Kerem Ege (2013). “Vibroacoustics of the Piano Soundboard: Reduced Models, Mobility Synthesis, and Acoustical Radiation Regime.”** *Journal of Sound and Vibration* 332(18), 4261–4279. [DOI: 10.1016/j.jsv.2013.03.015](https://doi.org/10.1016/j.jsv.2013.03.015). [Author-hosted preprint](https://arxiv.org/abs/1305.3057). Reduced position-dependent soundboard models.

15. **Benjamin Trévisan, Kerem Ege, and Bernard Laulagnet (2017). “A Modal Approach to Piano Soundboard Vibroacoustic Behavior.”** *Journal of the Acoustical Society of America* 141(2), 690–709. [DOI: 10.1121/1.4974860](https://doi.org/10.1121/1.4974860). [HAL full text](https://hal.science/hal-01456083/file/Trevisan_JASA2017_HAL.pdf). Physical upright-board modal modeling and excitation-position dependence.

### Full-model and evaluation references

16. **Juliette Chabassier, Antoine Chaigne, and Patrick Joly (2013). “Modeling and Simulation of a Grand Piano.”** *Journal of the Acoustical Society of America* 134(1), 648–665. [DOI: 10.1121/1.4809649](https://doi.org/10.1121/1.4809649). [INRIA research report/full preprint](https://inria.hal.science/file/index/docid/787551/filename/RR-8181.pdf). Comprehensive nonlinear string–soundboard–air reference model.

17. **International Telecommunication Union (2015). “Method for the Subjective Assessment of Intermediate Quality Level of Audio Systems.”** Recommendation ITU-R BS.1534-3. [Official ITU recommendation page](https://www.itu.int/rec/R-REC-BS.1534-3-201510-I/en) and [official full PDF](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.1534-3-201510-I!!PDF-E.pdf). Formal listening-test design, not a piano-specific quality metric.

18. **Meta Platforms. Meta Piano v1.0.0 corpus records.** [Official collection README](https://github.com/facebookincubator/Meta-open-sound/blob/main/README.md), [official manifest](https://raw.githubusercontent.com/facebookincubator/Meta-open-sound/main/systems/instruments/meta-piano/manifest.json), [release record](https://github.com/facebookincubator/Meta-open-sound/releases/tag/meta-piano-v1.0.0), and [CC0 license](https://github.com/facebookincubator/Meta-open-sound/blob/main/systems/instruments/meta-piano/LICENSE). Primary corpus structure, format, and licensing evidence; local provenance pins the exact archive checksum.
