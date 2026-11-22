#include "model/InstrumentModel.hpp"
#include "model/CalibrationData.hpp"
#include "model/SpectrumCalibration.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <stdexcept>

namespace pianocraft {
namespace {

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double ln1000 = 6.907755278982138;

void setPole(ModeCoefficients& mode, double frequency, double decaySeconds, double releaseSeconds,
             double residualSeconds, double sampleRate) {
    const double omega = 2.0 * pi * frequency / sampleRate;
    const double sustainRadius = std::exp(-6.907755278982138 / (decaySeconds * sampleRate));
    const double releaseRadius = std::exp(-6.907755278982138 / (releaseSeconds * sampleRate));
    mode.sustainA1 = 2.0 * sustainRadius * std::cos(omega);
    mode.sustainA2 = sustainRadius * sustainRadius;
    mode.releaseA1 = 2.0 * releaseRadius * std::cos(omega);
    mode.releaseA2 = releaseRadius * releaseRadius;
    // At least 2.5 times faster than the free string: Knight's residuals fall at 15-24 dB/s where its undamped
    // strings fall at 6-11 (VCSL sustains A2-A4).
    const double residualRadius = std::exp(-6.907755278982138 / (std::min(residualSeconds, decaySeconds / 2.5) * sampleRate));
    mode.residualA1 = 2.0 * residualRadius * std::cos(omega);
    mode.residualA2 = residualRadius * residualRadius;
    mode.frequency = frequency;
    mode.decaySeconds = decaySeconds;
    // A resonator driven by F per sample reaches F sin(omega) / (M w fs) times the modal
    // displacement's amplitude; baseStrike carries the rest (strike-point shape over M).
    mode.strike = mode.baseStrike * std::sin(omega) / (2.0 * pi * frequency * sampleRate);
    mode.slope = mode.baseSlope * std::sin(omega) / (2.0 * pi * frequency * sampleRate);
}

// Two-stage string decay from ungated early (0.05-0.4 s) and late (2-7 s) envelope slopes of
// partials 1-3 on all 88 keys, layer 3. The calibrated per-partial decays were ~1.8x too short
// and barely tracked the recordings (r=0.28). The gated refit in tools/evaluate_model.py accepts
// ~30% of partials and censors long decays; it scores the same on the complete 0.25-5 s envelope
// but 6 dB worse in the bass, so these laws stay (docs/model-laws.md).
double promptDecaySeconds(double frequency) {
    return 4.89 * std::pow(frequency / 440.0, -0.745);
}

// Weinreich's normal modes of two coupled strings, as frequency offset + i decay rate (rad/s,
// excluding intrinsic loss), and how a level strike (1, 1) splits between them at the bridge: the
// shares sum to 1. Exact for any mistuning, including 0, where the slow mode is silent.
struct NormalModes {
    std::complex<double> fast;
    std::complex<double> slow;
    std::complex<double> fastShare;
    std::complex<double> slowShare;
};

NormalModes unisonModes(double damping, double reactance, double mistuning) {
    const std::complex<double> coupling(reactance, damping);
    const auto root = std::sqrt(coupling * coupling + mistuning * mistuning / 4.0);
    NormalModes modes{coupling + root, coupling - root, {}, {}};
    if (modes.slow.imag() > modes.fast.imag()) {
        std::swap(modes.fast, modes.slow);
    }
    // Eigenvectors are (coupling, lambda - mistuning/2 - coupling).
    const auto fastSecond = modes.fast - mistuning / 2.0 - coupling;
    const auto slowSecond = modes.slow - mistuning / 2.0 - coupling;
    const auto determinant = coupling * (slowSecond - fastSecond);
    modes.fastShare = (slowSecond - coupling) / determinant * (coupling + fastSecond) / 2.0;
    modes.slowShare = (coupling - fastSecond) / determinant * (coupling + slowSecond) / 2.0;
    return modes;
}

// Longitudinal fundamental over f0, sqrt(ES / T). The E1 recording's strongest peak at ff is the
// phantom f6 + f7 at 532 Hz, 16 dB above partial 13 beside it: a longitudinal resonance at 13.06 f0.
// No other key shows one clearly, so the ratio is held across the keyboard (Analytic).
constexpr double longitudinalRatio = 13.06;
// Share of the longitudinal bridge force the board radiates: it pushes along the string, in the
// board's plane. Selected on the phantom partials' level (docs/model-laws.md).
constexpr double longitudinalCoupling = 0.03;

// ponytail: a fixed per-key, per-aspect hash (0-1) stands in for one particular old piano's wear,
// so the same Age gives the same piano on every render. Measure it from Lola's tape or Andrew's
// upright if those identify it.
double wear(int midi, std::uint32_t aspect) {
    std::uint32_t x = static_cast<std::uint32_t>(midi) * 2654435761u + aspect * 0x9E3779B9u;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x / 4294967295.0;
}

// A key's row of a fitted spectrum table (dB) at a frequency, between its 1/12-octave bands.
template <class Row>
double band(const Row& row, double frequency) {
    const double position = std::clamp(std::log2(frequency / spectrumLowest) * spectrumBandsPerOctave, 0.0,
                                       static_cast<double>(spectrumBands - 1));
    const auto below = std::min(static_cast<std::size_t>(position), spectrumBands - 2);
    return row[below] + (position - below) * (row[below + 1] - row[below]);
}

// Felt hysteresis: the rate-dependent share of the felt force at mid-compression, at 2 m/s.
// Selected on octave-band spectra: 0.25 scored better than 0 and 0.5 (docs/model-laws.md).
constexpr double feltHysteresis = 0.25;

double aftersoundDecaySeconds(double frequency) {
    return 16.87 * std::pow(frequency / 440.0, -0.444);
}

} // namespace

InstrumentModel::InstrumentModel(double sampleRate) {
    if (!std::isfinite(sampleRate) || sampleRate < 1000.0 || sampleRate > 768000.0) {
        throw std::invalid_argument("Unsupported sample rate");
    }
    sampleRate_ = sampleRate;

    static_assert(hasMeasuredCalibration);
    static_assert(measuredCalibration.size() == instrumentKeyCount);
    // Headroom for tuning up to +8% (466 Hz master tune with double stretch) below Nyquist.
    const double maximumFrequency = std::min(18000.0, sampleRate * 0.45);
    measuredA4_ = measuredCalibration[69 - 21].fundamental;
    for (std::size_t index = 0; index < keys_.size(); ++index) {
        auto& key = keys_[index];
        const auto& calibration = measuredCalibration[index];
        if (!std::isfinite(calibration.fundamental) || calibration.fundamental <= 0.0 ||
            !std::isfinite(calibration.inharmonicity) || calibration.inharmonicity < 0.0 ||
            !std::isfinite(calibration.decaySeconds) || calibration.decaySeconds <= 0.0 ||
            !std::isfinite(calibration.loudness) || calibration.loudness <= 0.0 ||
            !std::isfinite(calibration.hammerStiffness) || calibration.hammerStiffness <= 0.0 ||
            !std::isfinite(calibration.velocityGainExponent) || calibration.velocityGainExponent <= 0.0 ||
            !std::isfinite(calibration.velocityBrightnessExponent) || calibration.velocityBrightnessExponent < 0.0 ||
            !std::isfinite(calibration.hammerStiffnessScale) || calibration.hammerStiffnessScale <= 0.0 ||
            !std::isfinite(calibration.stereoLeftGain) || calibration.stereoLeftGain <= 0.0 ||
            !std::isfinite(calibration.stereoRightGain) || calibration.stereoRightGain <= 0.0) {
            throw std::invalid_argument("Invalid piano calibration data");
        }
        key.midi = static_cast<int>(index) + 21;
        key.fundamental = calibration.fundamental;
        key.stretchOctaves = std::log2(calibration.fundamental / measuredA4_) - (key.midi - 69) / 12.0;
        key.inharmonicity = calibration.inharmonicity;
        // Fitted to the comb of each key's recorded partials (tools/fit_piano.py strike).
        key.strikePosition = strikePositions[index];
        key.velocityGainExponent = calibration.velocityGainExponent;
        // Stulov's measured hammer set (Acta Acustica 91, 2005, as fitted in Stulov & Kartofelev,
        // Acoustics High Tatras 2009, eq. 9) for hammer n = 1-88: mass, felt exponent p and static
        // stiffness Q0 in N/mm^p. Its hysteresis fit leaves 0 < epsilon < 1 only for n < 11, so the
        // hysteresis time is selected instead: a fixed share of the time a 2 m/s strike takes to
        // compress the felt against a rigid stop (Stulov's eq. 6), so the loop keeps its shape
        // across the keyboard (docs/model-laws.md).
        {
            const double n = key.midi - 20.0;
            key.hammerMass = (11.074 - 0.074 * n + 1.0e-4 * n * n) * 1.0e-3;
            key.feltExponent = 3.7 + 0.015 * n;
            key.feltStiffness = 183.0 * std::exp(0.045 * n) * std::pow(1000.0, key.feltExponent);
            const double speed = 2.0;
            const double compression = std::pow((key.feltExponent + 1.0) * key.hammerMass * speed * speed /
                (2.0 * key.feltStiffness), 1.0 / (key.feltExponent + 1.0));
            key.feltRelaxation = feltHysteresis * compression / (key.feltExponent * speed);
        }
        // Analytic upright scale for the string's give under the hammer: 700 N on every string
        // (Bank & Sujbert's value), speaking length 5.2 cm at C8 growing 5.45% a semitone down and
        // foreshortened to 1.15 m in the bass. Modal mass is half the string's.
        const double tension = 700.0;
        const double length = std::min(1.15, 0.052 * std::pow(1.0545, 108 - key.midi));
        const double modalMass = 0.5 * tension / (4.0 * length * calibration.fundamental * calibration.fundamental);
        // Bridge force alone leaves the treble above D4 quieter than the recordings; this power law,
        // fitted against the reference corpus at pp/mf/ff, restores the key-to-key balance. The
        // 0.018 keeps a single velocity-127 note peaking near -12 dBFS at the default output gain (0.04
        // before the fitted spectrum tables, which raised the strings ~7 dB).
        const double levelLaw = std::pow(std::max(1.0, key.fundamental / 302.3), 1.042) * calibration.loudness;
        key.outputGain = 0.018 * levelLaw;
        // The law corrects radiated level, not the force on the bridge, so sympathetic strings get
        // half of it (in dB). Analytic register balance; without it bass notes' halo was 12 dB
        // lighter than in the (pedal-undocumented) recordings.
        key.sympatheticDrive = 1.0 / std::sqrt(levelLaw);
        key.hasDamper = key.midi <= 88;

        // While the hammer presses on the string every mode follows its push quasi-statically,
        // and that push reached the output as a low-frequency click 8-15 dB above the tone in
        // the treble. Real bridges feel far less of it (the recordings' attack is 16-23 dB down
        // below f0/2); a high-pass at 0.7 f0 matches that while costing the fundamental ~0.9 dB.
        {
            const double omega = 2.0 * pi * std::min(0.7 * key.fundamental, 0.45 * sampleRate) / sampleRate;
            const double alpha = std::sin(omega) / std::sqrt(2.0);
            const double a0 = 1.0 + alpha;
            const double b = (1.0 + std::cos(omega)) / 2.0 / a0;
            key.bridgeHighpass = {b, -2.0 * b, b, -2.0 * std::cos(omega) / a0, (1.0 - alpha) / a0};
        }


        const double baseDecay = calibration.decaySeconds;
        const double baseRelease = key.hasDamper
            ? std::clamp(0.12 + (84.0 - key.midi) * 0.009, 0.12, 0.75)
            : baseDecay;
        // The damped string's second stage (Selected on VCSL's Knight upright releases, A1-A5): its fundamental's
        // residual T60 3 s (20 dB/s), its upper partials shorter as their release is. The hand-over comes once
        // the fundamental has fallen 8 dB (C4: 45 ms), deeper towards the top damper (25 dB at E6), where
        // the recordings' fundamentals end 20-45 dB down. A single fast stage left C4 64 dB down in 0.3 s, where
        // the recordings hold a residual 13-18 dB down that fades at 9-25 dB/s.
        if (key.hasDamper) {
            key.residualRatio = 3.0 / baseRelease;
            const double handOver = 8.0 + 17.0 * std::clamp((key.midi - 76.0) / 12.0, 0.0, 1.0);
            key.residualDelay = handOver / 60.0 * baseRelease;
        }
        if (calibration.partialCount > calibrationPartialLimit) {
            throw std::invalid_argument("Too many calibrated partials");
        }
        std::size_t primaryCount = 0;
        for (std::size_t partialIndex = 0; partialIndex < calibration.partialCount; ++partialIndex) {
            const auto& partial = calibration.partials[partialIndex];
            if (partial.partial > 0 && partial.frequency > 0.0 && partial.frequency <= maximumFrequency &&
                partial.decaySeconds > 0.0) {
                ++primaryCount;
            }
        }
        if (primaryCount == 0) {
            continue;
        }
        const double normalization = 0.00011 / std::sqrt(static_cast<double>(primaryCount));
        std::array<std::size_t, calibrationPartialLimit> primaryIndices{};
        // Static compliance of the strike point (sum of shape^2 / (M w^2) over modes); the modes not
        // synthesized give way instantly, the synthesized ones through their resonators.
        // The strike's comb, sin(k pi x) averaged in power over strikes spread about x: the notches fill in up
        // the series, as the recordings' do (tools/fit_piano.py).
        const auto strikeShape = [&](double k) {
            const double spread = std::exp(-2.0 * std::pow(pi * k * strikeSpread, 2.0));
            const double power = 0.5 * (1.0 - std::cos(2.0 * pi * k * key.strikePosition) * spread);
            return std::copysign(std::sqrt(std::max(power, 0.0)), std::sin(k * pi * key.strikePosition));
        };
        const auto compliance = [&](double k, double frequency) {
            const double shape = strikeShape(k) * std::exp(-0.00045 * k * k);
            return shape * shape / (modalMass * std::pow(2.0 * pi * frequency, 2.0));
        };
        for (double k = 1.0; k <= 400.0; k += 1.0) {
            key.residualCompliance += compliance(k, k * key.fundamental *
                std::sqrt((1.0 + key.inharmonicity * k * k) / (1.0 + key.inharmonicity)));
        }
        const auto releaseSeconds = [baseRelease](double k) {
            // Upper partials damp a little faster: k^2 at 0.0003, where 0.0012 made Knight's releases' broadband
            // drop 5-8 dB deeper than their fundamental's, against 3 dB shallower in the recordings.
            return std::max(0.025, baseRelease / (1.0 + 0.007 * k + 0.0003 * k * k));
        };

        const auto addPrimary = [&](double k, double frequency) {
            auto& mode = key.modes[key.modeCount++];
            mode.baseFrequency = frequency;
            // The prompt law, by each partial's fitted factor (tools/fit_piano.py).
            mode.baseDecaySeconds = promptDecaySeconds(frequency) *
                (k <= spectrumPartials ? std::exp2(promptDecayLog2[index][static_cast<std::size_t>(k) - 1]) : 1.0);
            mode.baseReleaseSeconds = releaseSeconds(k);
            const double strikeWeight = strikeShape(k);
            const double finiteHammerWidth = std::exp(-0.00045 * k * k);
            mode.baseExcitation = normalization * strikeWeight * finiteHammerWidth;
            mode.baseStrike = strikeWeight * finiteHammerWidth / (modalMass * normalization);
            mode.baseSlope = k / (modalMass * normalization);
            key.residualCompliance -= compliance(k, frequency);
            // Bridge force in newtons (tension times string slope), 4 f0 sin(omega) / (N fs) per unit
            // of resonator output, through the bridge's falling mobility: one pole at a corner
            // selected at 500 Hz on octave-band spectra.
            // The fitted radiation (board and microphones) then weights each frequency of each key, and the
            // key's own measured partials what a smooth law leaves.
            const double own = k <= spectrumPartials ? partialDb[index][static_cast<std::size_t>(k) - 1] : 0.0;
            mode.drive = 4.0 * key.fundamental * std::sin(2.0 * pi * frequency / sampleRate) /
                (normalization * sampleRate) / (1.0 + frequency / 500.0);
            mode.output = mode.drive * std::pow(10.0, (band(radiationDb[index], frequency) + own) / 20.0);
            mode.softDb = band(velocityDb[index][0], frequency);
            mode.loudDb = band(velocityDb[index][1], frequency);
            mode.softBrighteningDb = band(brighteningDb[index][0], frequency);
            mode.loudBrighteningDb = band(brighteningDb[index][1], frequency);
            mode.secondary = false;
            ++key.primaryModeCount;
        };

        double highestPartial = 0.0;
        for (std::size_t partialIndex = 0; partialIndex < calibration.partialCount; ++partialIndex) {
            const auto& calibrated = calibration.partials[partialIndex];
            if (calibrated.partial == 0 || calibrated.frequency <= 0.0 ||
                calibrated.frequency > maximumFrequency || calibrated.decaySeconds <= 0.0) {
                continue;
            }
            primaryIndices[partialIndex] = key.modeCount;
            addPrimary(calibrated.partial, calibrated.frequency);
            highestPartial = std::max<double>(highestPartial, calibrated.partial);
        }

        // Calibration stops at 32 partials, so A0's series ended near 1 kHz while its recording
        // holds -13 to -16 dB of its energy above that. Continue every series to 9 kHz (stopping at 4
        // kHz left a wall in the bass's and mid's spectra) along the measured inharmonicity;
        // normalization stays tied to the calibrated count so levels hold.
        const double extensionLimit = std::min(9000.0, maximumFrequency);
        for (double k = highestPartial + 1.0; key.primaryModeCount < primaryModeLimit; k += 1.0) {
            const double frequency = k * key.fundamental *
                std::sqrt((1.0 + key.inharmonicity * k * k) / (1.0 + key.inharmonicity));
            if (frequency > extensionLimit) {
                break;
            }
            addPrimary(k, frequency);
        }
        key.residualCompliance = std::max(0.0, key.residualCompliance);

        // Longitudinal motion (Bank & Sujbert 2005): mode n feels -ES (n pi / L) / 2 times the
        // cosine projection of the squared slope, which pairs partials j, k with j + k = n or
        // |j - k| = n. Their simplification drives every longitudinal resonator with one odd and
        // one even mode's input; modes 1 and 2 give all phantoms from adjacent and next-but-one
        // partials: 2 sum(p_k p_k+1) + 2 (p_1^2 + 2 sum(p_k p_k+2)) with p_k = k q_k and the constant
        // ES pi^3 / (8 L^2), ES = ratio^2 T. Each mode's bridge force is ES (pi / L) n a_n.
        {
            const double stiffness = longitudinalRatio * longitudinalRatio * tension;
            key.longitudinalDrive = -stiffness * pi * pi * pi / (8.0 * length * length);
            for (std::size_t n = 1; n <= longitudinalLimit; ++n) {
                const double frequency = n * longitudinalRatio * key.fundamental;
                if (frequency > std::min(8000.0, 0.4 * sampleRate)) {
                    break;
                }
                auto& longitudinal = key.longitudinal[key.longitudinalCount++];
                longitudinal.baseFrequency = frequency;
                longitudinal.inverseMass = 1.0 / modalMass;
                longitudinal.drive = longitudinalCoupling * stiffness * pi / length * n * (n % 2 == 0 ? 1.0 : -1.0) /
                    (1.0 + frequency / 500.0);
                longitudinal.output = longitudinal.drive * std::pow(10.0, band(radiationDb[index], frequency) / 20.0);
            }
        }

        std::size_t secondaryCount = 0;
        for (std::size_t partialIndex = 0; partialIndex < calibration.partialCount; ++partialIndex) {
            const auto& calibrated = calibration.partials[partialIndex];
            if (!calibrated.secondaryAccepted || calibrated.frequency > maximumFrequency) {
                continue;
            }
            if (secondaryCount >= secondaryModeLimit || key.modeCount >= modeLimit ||
                calibrated.partial == 0 || calibrated.decaySeconds <= 0.0 ||
                calibrated.secondaryFrequency <= 0.0 || calibrated.secondaryFrequency > maximumFrequency ||
                calibrated.secondaryDecaySeconds <= 0.0 || calibrated.secondaryGain <= 0.0 ||
                calibrated.secondaryGain > 16.0) {
                throw std::invalid_argument("Invalid secondary calibration data");
            }
            const auto& primary = key.modes[primaryIndices[partialIndex]];
            auto& mode = key.modes[key.modeCount++];
            mode.baseFrequency = calibrated.secondaryFrequency;
            mode.baseDecaySeconds = calibrated.secondaryDecaySeconds;
            mode.baseReleaseSeconds = primary.baseDecaySeconds * 0.12;
            mode.baseExcitation = primary.baseExcitation * calibrated.secondaryGain;
            mode.output = primary.output * 0.9;
            mode.drive = primary.drive * 0.9;
            mode.softDb = primary.softDb;
            mode.loudDb = primary.loudDb;
            mode.softBrighteningDb = primary.softBrighteningDb;
            mode.loudBrighteningDb = primary.loudBrighteningDb;
            mode.secondary = true;
            ++secondaryCount;
        }

        // Partials without a calibrated secondary become a coupled unison pair (Weinreich). Its
        // two normal modes are fitted to the register-wide estimates: the prompt and aftersound
        // decay laws, a beat of ~0.5 Hz mid-keyboard (faster in the treble) and a slow mode 18 dB
        // down (~35 dB in the top octave). Analytic: calibrate.py accepted no per-partial
        // secondary, so these are not measured unisons. Unison Detune then scales the mistuning,
        // which is what makes the aftersound: at 0 it is gone, and more detune trades the two-stage
        // decay for beating.
        const double partnerDb = key.midi < 85 ? -18.0 : -18.0 - 17.0 * std::min(1.0, (key.midi - 85) / 15.0);
        for (std::size_t partialIndex = 0; partialIndex < calibration.partialCount; ++partialIndex) {
            const auto& calibrated = calibration.partials[partialIndex];
            if (calibrated.secondaryAccepted || calibrated.partial == 0 || calibrated.frequency <= 0.0 ||
                calibrated.frequency > maximumFrequency || calibrated.decaySeconds <= 0.0 ||
                secondaryCount >= secondaryModeLimit || key.modeCount >= modeLimit) {
                continue;
            }
            const double k = static_cast<double>(calibrated.partial);
            // The slow mode's level, by each partial's fitted offset (tools/fit_piano.py), at most -3 dB.
            const double partnerGain = std::pow(10.0, std::min(partnerDb + (k <= spectrumPartials
                ? aftersoundLevelDb[index][static_cast<std::size_t>(k) - 1] : 0.0), -3.0) / 20.0);
            // ponytail: golden-ratio hash for per-partial beat spread; deterministic, no RNG state.
            const double spread = std::fmod(k * 0.6180339887 + key.midi * 0.7548776662, 1.0);
            const double beatHz = (0.18 + 0.0006 * calibrated.frequency) * (0.6 + 0.8 * spread);
            const auto& primary = key.modes[primaryIndices[partialIndex]];
            auto& unison = key.unisons[key.unisonCount++];
            unison.prompt = primaryIndices[partialIndex];
            unison.partner = key.modeCount;
            // Invert the normal modes: with r + eta fixed by the two decay rates, the modes split by
            // w = pi beat + i (fast - slow) / 2, so kappa = Im(w^2) / (2 eta) and the mistuning
            // follows from w^2 = coupling^2 + delta^2 / 4. The slow share grows with eta; bisect it.
            const double fastRate = ln1000 / primary.baseDecaySeconds;
            const double aftersound = aftersoundDecaySeconds(calibrated.frequency) *
                (k <= spectrumPartials ? std::exp2(aftersoundDecayLog2[index][static_cast<std::size_t>(k) - 1]) : 1.0);
            const double slowRate = ln1000 / std::max(aftersound, primary.baseDecaySeconds);
            const double halfSplit = (fastRate - slowRate) / 2.0;
            const std::complex<double> split(pi * beatHz, halfSplit);
            const auto squared = split * split;
            double low = 0.0;
            double high = (fastRate + slowRate) / 2.0;
            for (int iteration = 0; iteration < 60; ++iteration) {
                unison.damping = 0.5 * (low + high);
                unison.reactance = squared.imag() / (2.0 * unison.damping);
                unison.mistuning = 2.0 * std::sqrt(std::max(0.0, squared.real() -
                    unison.reactance * unison.reactance + unison.damping * unison.damping));
                const auto modes = unisonModes(unison.damping, unison.reactance, unison.mistuning);
                (std::abs(modes.slowShare) < partnerGain * std::abs(modes.fastShare) ? low : high) = unison.damping;
            }
            unison.loss = (fastRate + slowRate) / 2.0 - unison.damping;
            if (spread >= 0.5) {
                unison.reactance = -unison.reactance; // the slow mode sits above instead
            }
            auto& mode = key.modes[key.modeCount++];
            mode.baseFrequency = calibrated.frequency;
            mode.baseReleaseSeconds = releaseSeconds(k);
            mode.baseExcitation = primary.baseExcitation;
            mode.output = primary.output;
            mode.drive = primary.drive;
            mode.softDb = primary.softDb;
            mode.loudDb = primary.loudDb;
            mode.softBrighteningDb = primary.softBrighteningDb;
            mode.loudBrighteningDb = primary.loudBrighteningDb;
            mode.secondary = true;
            ++secondaryCount;
        }
    }
    setVoicing(Voicing{});
}

void InstrumentModel::setVoicing(const Voicing& voicing) {
    for (std::size_t keyIndex = 0; keyIndex < keys_.size(); ++keyIndex) {
        auto& key = keys_[keyIndex];
        // Age wears the piano per key, all at Age 1 (Analytic: chosen by ear for an old home
        // upright, not measured):
        // - pitch slackened 25 cents with +-6 cents of unevenness, which also makes the stretch uneven;
        // - unisons drifted 1.5-4.5x apart;
        // - dampers leaking: release 1.5x on most keys, up to 5.5x on a few;
        // - hammers hardened 15% on average, +-35% key to key;
        // - key response uneven by +-2.5 dB.
        const double age = voicing.age;
        const double sag = age * (-25.0 + 6.0 * (2.0 * wear(key.midi, 0) - 1.0));
        const double unisonWear = 1.0 + age * (0.5 + 3.0 * wear(key.midi, 1));
        const double damperWear = 1.0 + age * (0.5 + 4.0 * std::pow(wear(key.midi, 2), 3.0));
        key.wearHardness = 1.0 + age * (0.15 + 0.35 * (2.0 * wear(key.midi, 3) - 1.0));
        key.wearGain = std::pow(10.0, age * 2.5 * (2.0 * wear(key.midi, 4) - 1.0) / 20.0);
        const double release = voicing.release * damperWear;
        // Every partial of a key moves together, so inharmonicity is kept.
        const double tuning = std::exp2((voicing.stretch - 1.0) * key.stretchOctaves + sag / 1200.0) *
            voicing.masterTune / measuredA4_;
        key.fundamental = measuredCalibration[keyIndex].fundamental * tuning;
        for (std::size_t index = 0; index < key.modeCount; ++index) {
            auto& mode = key.modes[index];
            setPole(mode, mode.baseFrequency * tuning, mode.baseDecaySeconds * voicing.sustain,
                    mode.baseReleaseSeconds * release, mode.baseReleaseSeconds * release * key.residualRatio, sampleRate_);
            mode.excitation = mode.baseExcitation * (mode.secondary ? voicing.aftersound : 1.0);
        }
        // Each coupled pair keeps its prompt mode on the measured pitch; the slow mode sits at the
        // normal modes' frequency difference and both carry their share's phase.
        for (std::size_t index = 0; index < key.unisonCount; ++index) {
            const auto& unison = key.unisons[index];
            const auto modes = unisonModes(unison.damping, unison.reactance, unison.mistuning * voicing.detune * unisonWear);
            auto& prompt = key.modes[unison.prompt];
            auto& partner = key.modes[unison.partner];
            setPole(prompt, prompt.baseFrequency * tuning, ln1000 / (unison.loss + modes.fast.imag()) * voicing.sustain,
                    prompt.baseReleaseSeconds * release, prompt.baseReleaseSeconds * release * key.residualRatio, sampleRate_);
            setPole(partner, (partner.baseFrequency + (modes.slow.real() - modes.fast.real()) / (2.0 * pi)) * tuning,
                    ln1000 / (unison.loss + modes.slow.imag()) * voicing.sustain,
                    partner.baseReleaseSeconds * release, partner.baseReleaseSeconds * release * key.residualRatio,
                    sampleRate_);
            prompt.excitation = prompt.baseExcitation * std::abs(modes.fastShare);
            prompt.phase = std::arg(modes.fastShare);
            partner.excitation = partner.baseExcitation * std::abs(modes.slowShare) * voicing.aftersound;
            partner.phase = std::arg(modes.slowShare);
        }
        key.strikeCompliance = 0.0;
        for (std::size_t index = 0; index < key.modeCount; ++index) {
            key.strikeCompliance += key.modes[index].strike * key.modes[index].excitation;
        }
        // Bank & Sujbert's longitudinal decay time, ~0.1 s, keeps each resonance broad.
        for (std::size_t index = 0; index < key.longitudinalCount; ++index) {
            auto& longitudinal = key.longitudinal[index];
            const double omega = 2.0 * pi * longitudinal.baseFrequency * tuning / sampleRate_;
            const double radius = std::exp(-ln1000 / (0.1 * sampleRate_));
            longitudinal.a1 = 2.0 * radius * std::cos(omega);
            longitudinal.a2 = radius * radius;
            longitudinal.input = longitudinal.inverseMass * std::sin(omega) / (omega * sampleRate_ * sampleRate_);
        }
    }
    setMicPosition(micPosition_);
}

void InstrumentModel::setMicPosition(MicPosition position) {
    micPosition_ = position;
    for (std::size_t index = 0; index < keys_.size(); ++index) {
        auto& key = keys_[index];
        const auto& calibration = measuredCalibration[index];
        // The spaced pair keeps the calibrated pan; the coincident pair and the player's ears
        // hear the keyboard spread wider, bass left and treble right.
        const double width = position == MicPosition::spacedPair ? 0.55 : 0.9;
        const double pan = std::clamp((key.midi - 64.5) / 55.0, -width, width);
        key.leftGain = std::sqrt(0.5 * (1.0 - pan)) * calibration.stereoLeftGain;
        key.rightGain = std::sqrt(0.5 * (1.0 + pan)) * calibration.stereoRightGain;

        // Each partial reaches the right channel with its own phase, built from the resonator's
        // current and previous outputs:
        // - spaced pair: diffuse phase above 100 Hz, matching the reference recordings' ~0 L/R
        //   correlation while the deep bass stays mono-compatible;
        // - coincident: no phase difference, so the image comes from level alone (mono-safe);
        // - player: up to 0.3 ms interaural delay toward the ends of the keyboard, plus diffuse
        //   soundboard radiation above ~1 kHz.
        for (std::size_t modeIndex = 0; modeIndex < key.modeCount; ++modeIndex) {
            auto& mode = key.modes[modeIndex];
            const double spread = std::fmod(modeIndex * 0.7548776662 + key.midi * 0.5698402910, 1.0) - 0.5;
            double phase = 0.0;
            if (position == MicPosition::spacedPair) {
                phase = 2.0 * pi * spread * std::min(1.0, mode.frequency / 100.0);
            } else if (position == MicPosition::player) {
                phase = 2.0 * pi * mode.frequency * 0.0003 * pan / width +
                    2.0 * pi * spread * std::clamp((mode.frequency - 1000.0) / 2000.0, 0.0, 1.0);
            }
            // A phase shift is a mix of the current and previous outputs.
            const double omega = 2.0 * pi * mode.frequency / sampleRate_;
            const auto rotate = [&](double weight, double shift, double& current, double& previous) {
                current = weight * (std::cos(shift) + std::sin(shift) * std::cos(omega) / std::sin(omega));
                previous = -weight * std::sin(shift) / std::sin(omega);
            };
            rotate(mode.output, mode.phase, mode.outputLeft, mode.outputLeftPrevious);
            rotate(mode.output, mode.phase + phase, mode.outputRight, mode.outputRightPrevious);
            rotate(mode.drive, mode.phase, mode.driveCurrent, mode.drivePrevious);
        }
    }
}

const KeyModel& InstrumentModel::key(int midi) const {
    if (midi < 21 || midi > 108) {
        throw std::out_of_range("Piano key is outside A0-C8");
    }
    return keys_[static_cast<std::size_t>(midi - 21)];
}

} // namespace pianocraft
