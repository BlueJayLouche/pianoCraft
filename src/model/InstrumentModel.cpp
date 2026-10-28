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

} // namespace

InstrumentModel::InstrumentModel(double sampleRate) {
    if (!std::isfinite(sampleRate) || sampleRate < 1000.0 || sampleRate > 768000.0) {
        throw std::invalid_argument("Unsupported sample rate");
    }

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
        key.outputGain = std::clamp(0.55 + (key.midi - 36) * 0.025, 0.55, 1.0) * calibration.loudness;
        key.hasDamper = key.midi <= 88;

        const double pan = std::clamp((key.midi - 64.5) / 55.0, -0.55, 0.55);
        key.leftGain = std::sqrt(0.5 * (1.0 - pan)) * calibration.stereoLeftGain;
        key.rightGain = std::sqrt(0.5 * (1.0 + pan)) * calibration.stereoRightGain;

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

        for (std::size_t partialIndex = 0; partialIndex < calibration.partialCount; ++partialIndex) {
            const auto& calibrated = calibration.partials[partialIndex];
            if (calibrated.partial == 0 || calibrated.frequency <= 0.0 ||
                calibrated.frequency > maximumFrequency || calibrated.decaySeconds <= 0.0) {
                continue;
            }
            const double k = static_cast<double>(calibrated.partial);
            const double release = std::max(0.025, baseRelease /
                (1.0 + 0.014 * k + 0.0012 * k * k));
            primaryIndices[partialIndex] = key.modeCount;
            auto& mode = key.modes[key.modeCount++];
            setPole(mode, calibrated.frequency, calibrated.decaySeconds, release, sampleRate);
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
    }
}

const KeyModel& InstrumentModel::key(int midi) const {
    if (midi < 21 || midi > 108) {
        throw std::out_of_range("Piano key is outside A0-C8");
    }
    return keys_[static_cast<std::size_t>(midi - 21)];
}

} // namespace pianocraft
