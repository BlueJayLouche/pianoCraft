#include "engine/SympatheticBank.hpp"

#include <algorithm>
#include <cmath>

namespace pianocraft {

void SympatheticBank::prepare(double sampleRate, const InstrumentModel& model) {
    constexpr double pi = 3.141592653589793238462643383279502884;
    for (std::size_t index = 0; index < instrumentKeyCount; ++index) {
        const auto& key = model.key(static_cast<int>(index) + 21);
        for (std::size_t partialIndex = 0; partialIndex < modesPerKey; ++partialIndex) {
            const double partial = static_cast<double>(partialIndex + 1);
            const double frequency = partial * key.fundamental *
                std::sqrt((1.0 + key.inharmonicity * partial * partial) /
                          (1.0 + key.inharmonicity));
            auto& mode = keys_[index][partialIndex];
            if (frequency >= sampleRate * 0.45) {
                mode = {};
                continue;
            }
            const double decay = std::max(0.2, key.modes[0].decaySeconds / (1.0 + 0.2 * partialIndex));
            const double release = std::max(0.035, decay * 0.045);
            const double omega = 2.0 * pi * frequency / sampleRate;
            const double sustainRadius = std::exp(-6.907755278982138 / (decay * sampleRate));
            const double releaseRadius = std::exp(-6.907755278982138 / (release * sampleRate));
            mode.sustainA1 = 2.0 * sustainRadius * std::cos(omega);
            mode.sustainA2 = sustainRadius * sustainRadius;
            mode.releaseA1 = 2.0 * releaseRadius * std::cos(omega);
            mode.releaseA2 = releaseRadius * releaseRadius;
            mode.inputGain = 0.15 / (1.0 + partialIndex * 0.7);
            mode.outputGain = 0.06 * (1.0 - sustainRadius) / (1.0 + partialIndex * 0.5);
        }
    }
    reset();
}

void SympatheticBank::reset() {
    for (auto& key : keys_) {
        for (auto& mode : key) {
            mode.y1 = 0.0;
            mode.y2 = 0.0;
        }
    }
}

std::array<double, 3> SympatheticBank::process(
    const std::array<double, 3>& input,
    const std::array<bool, instrumentKeyCount>& undamped,
    double level) {
    std::array<double, 3> output{};
    for (std::size_t index = 0; index < instrumentKeyCount; ++index) {
        const std::size_t region = index + 21 < 48 ? 0 : (index + 21 < 72 ? 1 : 2);
        for (auto& mode : keys_[index]) {
            if (mode.inputGain == 0.0) {
                continue;
            }
            const double a1 = undamped[index] ? mode.sustainA1 : mode.releaseA1;
            const double a2 = undamped[index] ? mode.sustainA2 : mode.releaseA2;
            const double value = input[region] * mode.inputGain + a1 * mode.y1 - a2 * mode.y2;
            mode.y2 = mode.y1;
            mode.y1 = std::abs(value) < 1.0e-30 ? 0.0 : value;
            output[region] += mode.y1 * mode.outputGain * level;
        }
    }
    return output;
}

} // namespace pianocraft
