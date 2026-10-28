#include "engine/Soundboard.hpp"

#include "model/CalibrationData.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pianocraft {

void Soundboard::prepare(double sampleRate) {
    constexpr double pi = 3.141592653589793238462643383279502884;
    constexpr std::array<double, modesPerRegion> frequencies{
        72.0, 103.0, 149.0, 213.0, 307.0, 441.0, 635.0,
        914.0, 1316.0, 1895.0, 2728.0, 3928.0, 5656.0, 8144.0,
    };
    for (std::size_t region = 0; region < regionCount; ++region) {
        const double direct = soundboardCalibration.directGain[region];
        const double resonance = soundboardCalibration.resonanceGain[region];
        const double tilt = soundboardCalibration.spectralTilt[region];
        if (!std::isfinite(direct) || direct < 0.5 || direct > 1.5 ||
            !std::isfinite(resonance) || resonance < 0.0 || resonance > 2.0 ||
            !std::isfinite(tilt) || tilt < -1.0 || tilt > 1.0) {
            throw std::invalid_argument("Invalid soundboard calibration data");
        }
        for (std::size_t index = 0; index < modesPerRegion; ++index) {
            auto& mode = regions_[region][index];
            const double frequency = frequencies[index] * (1.0 + 0.018 * (region - 1.0));
            if (frequency >= sampleRate * 0.45) {
                mode = {};
                continue;
            }
            const double decay = 0.055 + 0.42 / (1.0 + frequency / 240.0);
            const double radius = std::exp(-6.907755278982138 / (decay * sampleRate));
            const double omega = 2.0 * pi * frequency / sampleRate;
            mode.a1 = 2.0 * radius * std::cos(omega);
            mode.a2 = radius * radius;
            const double radiation = std::clamp((frequency - 45.0) / 180.0, 0.08, 1.0);
            const double gain = 0.06 * (1.0 - radius) * radiation * resonance *
                std::pow(frequency / 500.0, tilt) / std::sqrt(1.0 + frequency / 900.0);
            mode.gainLeft = gain * (1.0 + 0.08 * std::sin(index * 1.7 + region));
            mode.gainRight = gain * (1.0 + 0.08 * std::cos(index * 1.3 + region));
        }
    }
    reset();
}

void Soundboard::reset() {
    for (auto& region : regions_) {
        for (auto& mode : region) {
            mode.y1Left = 0.0;
            mode.y2Left = 0.0;
            mode.y1Right = 0.0;
            mode.y2Right = 0.0;
        }
    }
}

void Soundboard::process(const std::array<double, 3>& inputLeft,
                         const std::array<double, 3>& inputRight,
                         double bodyLevel, double& outputLeft, double& outputRight) {
    outputLeft = 0.0;
    outputRight = 0.0;
    for (std::size_t region = 0; region < regionCount; ++region) {
        double bodyLeft = 0.0;
        double bodyRight = 0.0;
        for (auto& mode : regions_[region]) {
            const double left = inputLeft[region] + mode.a1 * mode.y1Left - mode.a2 * mode.y2Left;
            const double right = inputRight[region] + mode.a1 * mode.y1Right - mode.a2 * mode.y2Right;
            mode.y2Left = mode.y1Left;
            mode.y1Left = std::abs(left) < 1.0e-30 ? 0.0 : left;
            mode.y2Right = mode.y1Right;
            mode.y1Right = std::abs(right) < 1.0e-30 ? 0.0 : right;
            bodyLeft += mode.y1Left * mode.gainLeft;
            bodyRight += mode.y1Right * mode.gainRight;
        }
        const double direct = 0.72 * soundboardCalibration.directGain[region];
        outputLeft += inputLeft[region] * direct + bodyLeft * bodyLevel;
        outputRight += inputRight[region] * direct + bodyRight * bodyLevel;
    }
}

} // namespace pianocraft
