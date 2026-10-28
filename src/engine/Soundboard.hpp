#pragma once

#include <array>
#include <cstddef>

namespace pianocraft {

class Soundboard {
public:
    void prepare(double sampleRate);
    void reset();
    void process(const std::array<double, 3>& inputLeft,
                 const std::array<double, 3>& inputRight,
                 double bodyLevel, double& outputLeft, double& outputRight);

private:
    struct Mode {
        double a1 = 0.0;
        double a2 = 0.0;
        double gainLeft = 0.0;
        double gainRight = 0.0;
        double y1Left = 0.0;
        double y2Left = 0.0;
        double y1Right = 0.0;
        double y2Right = 0.0;
    };

    static constexpr std::size_t regionCount = 3;
    static constexpr std::size_t modesPerRegion = 14;
    std::array<std::array<Mode, modesPerRegion>, regionCount> regions_{};
};

} // namespace pianocraft
