#pragma once

#include "model/InstrumentModel.hpp"

#include <array>
#include <cstddef>

namespace pianocraft {

class Soundboard {
public:
    // The board of one piano (its BodyCalibration, model/PianoData.hpp).
    void prepare(double sampleRate, const BodyCalibration& calibration);
    void reset();
    void setMicPosition(MicPosition position);
    // The centre output is one coherent microphone (the left one's view of the board), for mono.
    void process(const std::array<double, 3>& directLeft,
                 const std::array<double, 3>& directRight,
                 const std::array<double, 3>& directCentre,
                 const std::array<double, 3>& bridge,
                 double bodyLevel, double& outputLeft, double& outputRight, double& outputCentre);

private:
    static constexpr std::size_t regionCount = 3;
    static constexpr std::size_t modeLimit = 96;

    struct Mode {
        double a1 = 0.0;
        double a2 = 0.0;
        std::array<double, regionCount> input{};
        double frequency = 0.0;
        double shapeLeft = 0.0;
        double shapeRight = 0.0;
        double gainLeft = 0.0;
        double gainRight = 0.0;
        double y1 = 0.0;
        double y2 = 0.0;
    };

    std::array<Mode, modeLimit> modes_{};
    std::size_t modeCount_ = 0;
    std::array<double, regionCount> directGain_{};
};

} // namespace pianocraft
