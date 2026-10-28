#include "engine/SympatheticBank.hpp"

#include <algorithm>
#include <cmath>

namespace pianocraft {

void SympatheticBank::prepare(double sampleRate, const InstrumentModel& model) {
    constexpr double pi = 3.141592653589793238462643383279502884;
    // Every string resonates at its own first measured partials and hears the whole board, so a
    // treble note can set bass strings ringing. Levels are analytic: the reference recordings don't
    // document pedal state, so their between-partial energy is only a plausibility check.
    keys_ = {};
    for (std::size_t index = 0; index < instrumentKeyCount; ++index) {
        const auto& key = model.key(static_cast<int>(index) + 21);
        std::size_t partialIndex = 0;
        for (std::size_t modeIndex = 0; modeIndex < key.modeCount && partialIndex < modesPerKey; ++modeIndex) {
            const auto& string = key.modes[modeIndex];
            if (string.secondary || string.frequency >= sampleRate * 0.45) {
                continue;
            }
            auto& mode = keys_[index][partialIndex];
            const double decay = string.decaySeconds;
            const double release = std::max(0.035, decay * 0.045);
            const double omega = 2.0 * pi * string.frequency / sampleRate;
            const double sustainRadius = std::exp(-6.907755278982138 / (decay * sampleRate));
            const double releaseRadius = std::exp(-6.907755278982138 / (release * sampleRate));
            mode.sustainA1 = 2.0 * sustainRadius * std::cos(omega);
            mode.sustainA2 = sustainRadius * sustainRadius;
            mode.releaseA1 = 2.0 * releaseRadius * std::cos(omega);
            mode.releaseA2 = releaseRadius * releaseRadius;
            mode.inputGain = 0.15 / (1.0 + partialIndex * 0.7);
            mode.outputGain = 1.2 * (1.0 - sustainRadius) / (1.0 + partialIndex * 0.5);
            ++partialIndex;
        }
    }
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
    const std::array<double, instrumentKeyCount>& undamped,
    double level) {
    const double board = input[0] + input[1] + input[2];
    std::array<double, 3> output{};
    for (std::size_t index = 0; index < instrumentKeyCount; ++index) {
        const std::size_t region = index + 21 < 48 ? 0 : (index + 21 < 72 ? 1 : 2);
        const double open = undamped[index];
        for (auto& mode : keys_[index]) {
            // Dampers block the drive in proportion to their contact; a fully damped string only
            // rings out what it already holds.
            if (mode.inputGain == 0.0 || (open == 0.0 && mode.y1 == 0.0 && mode.y2 == 0.0)) {
                continue;
            }
            // Exact sustain or release coefficients at open = 1 or 0.
            const double a1 = mode.sustainA1 * open + mode.releaseA1 * (1.0 - open);
            const double a2 = mode.sustainA2 * open + mode.releaseA2 * (1.0 - open);
            const double value = board * mode.inputGain * open + a1 * mode.y1 - a2 * mode.y2;
            mode.y2 = mode.y1;
            mode.y1 = std::abs(value) < 1.0e-30 ? 0.0 : value;
            output[region] += mode.y1 * mode.outputGain * level;
        }
    }
    return output;
}

} // namespace pianocraft
