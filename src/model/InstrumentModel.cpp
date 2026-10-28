#include "model/InstrumentModel.hpp"
#include "model/CalibrationData.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pianocraft {
namespace {

constexpr double pi = 3.141592653589793238462643383279502884;

void setPole(ModeCoefficients& mode, double frequency, double decaySeconds,
             double releaseSeconds, double sampleRate) {
    const double omega = 2.0 * pi * frequency / sampleRate;
    const double sustainRadius = std::exp(-6.907755278982138 / (decaySeconds * sampleRate));
    const double releaseRadius = std::exp(-6.907755278982138 / (releaseSeconds * sampleRate));
    mode.sustainA1 = 2.0 * sustainRadius * std::cos(omega);
    mode.sustainA2 = sustainRadius * sustainRadius;
    mode.releaseA1 = 2.0 * releaseRadius * std::cos(omega);
    mode.releaseA2 = releaseRadius * releaseRadius;
    mode.frequency = frequency;
    mode.decaySeconds = decaySeconds;
}

// Two-stage string decay fitted to the reference corpus: early (0.05-0.4 s) and late (2-7 s)
// envelope slopes of partials 1-3 on all 88 keys, layer 3. The calibrated per-partial decays
// were ~1.8x too short and their scatter barely tracked the recordings (r=0.28), so they are
// replaced by these register laws.
double promptDecaySeconds(double frequency) {
    return 4.89 * std::pow(frequency / 440.0, -0.745);
}

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
    const double maximumFrequency = std::min(18000.0, sampleRate * 0.45);
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
        key.inharmonicity = calibration.inharmonicity;
        key.strikePosition = std::clamp(0.115 + (key.midi - 60) * 0.00045, 0.075, 0.145);
        key.hammerStiffness = calibration.hammerStiffness;
        key.velocityGainExponent = calibration.velocityGainExponent;
        key.velocityBrightnessExponent = calibration.velocityBrightnessExponent;
        key.hammerStiffnessScale = calibration.hammerStiffnessScale;
        // The modal strings lose roughly 9 dB per octave up the keyboard; this power law, fitted
        // against the reference corpus at pp/mf/ff, restores the recorded key-to-key balance.
        // The 0.3 keeps a single velocity-127 note near -6 dBFS at the default output gain.
        key.outputGain = 0.3 * std::pow(std::max(1.0, key.fundamental / 55.0), 1.47) * calibration.loudness;
        key.hasDamper = key.midi <= 88;


        const double baseDecay = calibration.decaySeconds;
        const double baseRelease = key.hasDamper
            ? std::clamp(0.12 + (84.0 - key.midi) * 0.009, 0.12, 0.75)
            : baseDecay;
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
        const auto releaseSeconds = [baseRelease](double k) {
            return std::max(0.025, baseRelease / (1.0 + 0.014 * k + 0.0012 * k * k));
        };

        for (std::size_t partialIndex = 0; partialIndex < calibration.partialCount; ++partialIndex) {
            const auto& calibrated = calibration.partials[partialIndex];
            if (calibrated.partial == 0 || calibrated.frequency <= 0.0 ||
                calibrated.frequency > maximumFrequency || calibrated.decaySeconds <= 0.0) {
                continue;
            }
            const double k = static_cast<double>(calibrated.partial);
            primaryIndices[partialIndex] = key.modeCount;
            auto& mode = key.modes[key.modeCount++];
            setPole(mode, calibrated.frequency, promptDecaySeconds(calibrated.frequency), releaseSeconds(k),
                    sampleRate);
            const double strikeWeight = std::sin(k * pi * key.strikePosition);
            const double finiteHammerWidth = std::exp(-0.00045 * k * k);
            mode.excitation = normalization * strikeWeight * finiteHammerWidth;
            mode.output = 1.0 / (1.0 + 0.018 * k);
            mode.secondary = false;
            ++key.primaryModeCount;
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
            setPole(mode, calibrated.secondaryFrequency, calibrated.secondaryDecaySeconds,
                    primary.decaySeconds * 0.12, sampleRate);
            mode.excitation = primary.excitation * calibrated.secondaryGain;
            mode.output = primary.output * 0.9;
            mode.secondary = true;
            ++secondaryCount;
        }

        // Partials without a calibrated secondary get an effective unison partner: slightly
        // detuned (the recordings beat at ~0.5 Hz mid-keyboard, faster in the treble), 18 dB down,
        // decaying at the aftersound rate. It supplies the beating and the long tail after the
        // prompt decay. The top octave's tail is quieter in the recordings (~-35 dB).
        const double partnerDb = key.midi < 85 ? -18.0 : -18.0 - 17.0 * std::min(1.0, (key.midi - 85) / 15.0);
        const double partnerGain = std::pow(10.0, partnerDb / 20.0);
        for (std::size_t partialIndex = 0; partialIndex < calibration.partialCount; ++partialIndex) {
            const auto& calibrated = calibration.partials[partialIndex];
            if (calibrated.secondaryAccepted || calibrated.partial == 0 || calibrated.frequency <= 0.0 ||
                calibrated.frequency > maximumFrequency || calibrated.decaySeconds <= 0.0 ||
                secondaryCount >= secondaryModeLimit || key.modeCount >= modeLimit) {
                continue;
            }
            const double k = static_cast<double>(calibrated.partial);
            // ponytail: golden-ratio hash for per-partial beat spread; deterministic, no RNG state.
            const double spread = std::fmod(k * 0.6180339887 + key.midi * 0.7548776662, 1.0);
            const double beatHz = (0.18 + 0.0006 * calibrated.frequency) * (0.6 + 0.8 * spread);
            const auto& primary = key.modes[primaryIndices[partialIndex]];
            auto& mode = key.modes[key.modeCount++];
            setPole(mode, calibrated.frequency + (spread < 0.5 ? beatHz : -beatHz),
                    std::max(aftersoundDecaySeconds(calibrated.frequency), primary.decaySeconds),
                    releaseSeconds(k), sampleRate);
            mode.excitation = primary.excitation * partnerGain;
            mode.output = primary.output;
            mode.secondary = true;
            ++secondaryCount;
        }
    }
    setMicPosition(MicPosition::spacedPair);
}

void InstrumentModel::setMicPosition(MicPosition position) {
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
            const double omega = 2.0 * pi * mode.frequency / sampleRate_;
            mode.outputRight = mode.output * (std::cos(phase) + std::sin(phase) * std::cos(omega) / std::sin(omega));
            mode.outputRightPrevious = -mode.output * std::sin(phase) / std::sin(omega);
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
