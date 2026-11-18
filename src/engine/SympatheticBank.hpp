#pragma once

#include "model/InstrumentModel.hpp"

#include <array>
#include <cstddef>

namespace pianocraft {

class SympatheticBank {
public:
    void prepare(double sampleRate, const InstrumentModel& model);
    // Follows the strings' current tuning and decay without silencing what is ringing.
    void update(double sampleRate, const InstrumentModel& model);
    void reset();
    std::array<double, 3> process(const std::array<double, 3>& input,
                                  const std::array<double, instrumentKeyCount>& undamped,
                                  double level);

private:
    struct Mode {
        double sustainA1 = 0.0;
        double sustainA2 = 0.0;
        double releaseA1 = 0.0;
        double releaseA2 = 0.0;
        double inputGain = 0.0;
        double outputGain = 0.0;
        double y1 = 0.0;
        double y2 = 0.0;
    };

    static constexpr std::size_t modesPerKey = 8;
    std::array<std::array<Mode, modesPerKey>, instrumentKeyCount> keys_{};
    std::array<double, instrumentKeyCount> open_{}; // undamped, ramped as the dampers move
    double damperStep_ = 1.0;
};

} // namespace pianocraft
