#include "engine/Soundboard.hpp"

#include "model/CalibrationData.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace pianocraft {

void Soundboard::prepare(double sampleRate) {
    constexpr double pi = 3.141592653589793238462643383279502884;
    for (std::size_t region = 0; region < regionCount; ++region) {
        const double direct = soundboardCalibration.directGain[region];
        const double resonance = soundboardCalibration.resonanceGain[region];
        const double tilt = soundboardCalibration.spectralTilt[region];
        if (!std::isfinite(direct) || direct < 0.5 || direct > 1.5 ||
            !std::isfinite(resonance) || resonance < 0.0 || resonance > 2.0 ||
            !std::isfinite(tilt) || tilt < -1.0 || tilt > 1.0) {
            throw std::invalid_argument("Invalid soundboard calibration data");
        }
    }

    // One shared board driven at three bridge regions. Modes sit ~22 Hz apart below ~370 Hz and
    // about a semitone apart above, where Q=40 (2.5% loss) makes neighbours overlap. Each region
    // and each microphone sees a different mode-shape amplitude, so registers colour differently
    // and the body radiates decorrelated stereo. A region drives modes far below its lowest note
    // only weakly; otherwise every treble strike rings the 70-200 Hz modes louder than the note.
    constexpr std::array<double, regionCount> regionLowest{27.5, 130.81, 523.25};
    constexpr double q = 40.0;
    // ponytail: fixed-seed LCG stands in for measured mode shapes; fit them if bridge or
    // radiation measurements become available.
    std::uint32_t seed = 2654435769u;
    const auto random = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) * (1.0 / 16777216.0);
    };
    modes_ = {};
    modeCount_ = 0;
    const double maximumFrequency = std::min(10000.0, sampleRate * 0.45);
    for (double frequency = 50.0; frequency < maximumFrequency && modeCount_ < modeLimit;
         frequency += std::max(22.0, 0.0595 * frequency) * (0.6 + 0.8 * random())) {
        auto& mode = modes_[modeCount_++];
        const double radius = std::exp(-pi * frequency / (q * sampleRate));
        const double omega = 2.0 * pi * frequency / sampleRate;
        mode.a1 = 2.0 * radius * std::cos(omega);
        mode.a2 = radius * radius;
        const double unitPeak = (1.0 - radius) * 2.0 * std::sin(omega);
        for (std::size_t region = 0; region < regionCount; ++region) {
            mode.input[region] = unitPeak * (2.0 * random() - 1.0) *
                frequency / (frequency + regionLowest[region]) *
                soundboardCalibration.resonanceGain[region] *
                std::pow(frequency / 500.0, soundboardCalibration.spectralTilt[region]);
        }
        const double radiation = std::clamp((frequency - 45.0) / 180.0, 0.08, 1.0) /
            std::sqrt(1.0 + frequency / 900.0);
        mode.frequency = frequency;
        mode.shapeLeft = radiation * (2.0 * random() - 1.0);
        mode.shapeRight = radiation * (2.0 * random() - 1.0);
    }
    setMicPosition(MicPosition::spacedPair);
}

// Spaced microphones see each mode with an unrelated amplitude; coincident ones see the same;
// ears are close enough to hear the same below ~1 kHz.
void Soundboard::setMicPosition(MicPosition position) {
    for (std::size_t index = 0; index < modeCount_; ++index) {
        auto& mode = modes_[index];
        const double independence = position == MicPosition::spacedPair ? 1.0
            : position == MicPosition::coincident ? 0.0
            : std::min(1.0, mode.frequency / 1000.0);
        mode.gainLeft = mode.shapeLeft;
        mode.gainRight = mode.shapeLeft + (mode.shapeRight - mode.shapeLeft) * independence;
    }
}

void Soundboard::reset() {
    for (auto& mode : modes_) {
        mode.y1 = 0.0;
        mode.y2 = 0.0;
    }
}

void Soundboard::process(const std::array<double, 3>& directLeft,
                         const std::array<double, 3>& directRight,
                         const std::array<double, 3>& directCentre,
                         const std::array<double, 3>& bridge,
                         double bodyLevel, double& outputLeft, double& outputRight, double& outputCentre) {
    double bodyLeft = 0.0;
    double bodyRight = 0.0;
    for (std::size_t index = 0; index < modeCount_; ++index) {
        auto& mode = modes_[index];
        const double value = mode.input[0] * bridge[0] + mode.input[1] * bridge[1] +
            mode.input[2] * bridge[2] + mode.a1 * mode.y1 - mode.a2 * mode.y2;
        mode.y2 = mode.y1;
        mode.y1 = std::abs(value) < 1.0e-30 ? 0.0 : value;
        bodyLeft += mode.y1 * mode.gainLeft;
        bodyRight += mode.y1 * mode.gainRight;
    }
    outputLeft = bodyLeft * bodyLevel;
    outputRight = bodyRight * bodyLevel;
    outputCentre = outputLeft;
    for (std::size_t region = 0; region < regionCount; ++region) {
        const double direct = 0.72 * soundboardCalibration.directGain[region];
        outputLeft += directLeft[region] * direct;
        outputRight += directRight[region] * direct;
        outputCentre += directCentre[region] * direct;
    }
}

} // namespace pianocraft
