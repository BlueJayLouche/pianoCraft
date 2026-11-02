#pragma once

#include "engine/Filters.hpp"
#include "engine/MonoWind.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace pianocraft {

using FluteParameterId = WindParameterId;

// A flute: a jet-drive waveguide (Fletcher; Verge), played by MonoWind. See docs/harp-and-flute.md.
class FluteEngine : public MonoWind {
public:
    FluteEngine();
    void prepare(double sampleRate);
    void reset();
    void render(float* left, float* right, std::uint32_t frameCount);

private:
    static constexpr std::size_t delaySize = 4096;

    void silence() override;

    std::array<double, delaySize> bore_{};
    std::array<double, delaySize> jet_{};
    std::size_t write_ = 0;
    double reflection_ = 0.0;   // one-pole low-pass state: the open end's losses
    double jetLow_ = 0.0;       // the jet's own low-pass
    double dcInput_ = 0.0;      // DC blocker on the bore input
    double dcOutput_ = 0.0;
    double radiated_ = 0.0;     // the output high-pass's low band
    std::array<double, 4> formant_{}; // Fletcher's formant: high-pass then low-pass, two states each
    double boreLevel_ = 0.0;           // the bore wave's mean square, for the labium split
    filters::VoicingStage voicing_;    // the fitted output stage
    double noiseLow_ = 0.0;
    double airLow_ = 0.0;
    double airHigh_ = 0.0;
    std::uint32_t noise_ = 0x9E3779B9u;
};

} // namespace pianocraft
