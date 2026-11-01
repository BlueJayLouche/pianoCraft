#pragma once

#include "engine/MonoWind.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace pianocraft {

using ReedParameterId = WindParameterId;

// What makes one reed instrument differ from another.
struct ReedModel {
    int lowestKey;
    int highestKey;
    // A cylindrical bore (clarinet: closed at the reed, open at the bell, so only odd harmonics
    // resonate) or a conical one (oboe: every harmonic), taken as a cylinder open at both ends with
    // the reed this share of the way along it, where the cone's missing tip would be.
    bool conical;
    double apex;
    double reedOffset; // the reed's reflection at rest, and how fast pressure across it closes it
    double reedSlope;
    double loss;       // the bell's reflection low-pass, pole at 48 kHz
    double toneHoles;  // the tone-hole lattice's cutoff (Hz): above it the bore radiates freely
    std::array<std::array<double, 3>, 2> formants; // radiated resonances: Hz, Q, gain
    double vibrato;    // Vibrato Depth by default
    // The mouth's pressure, as a share of what shuts the reed, at the lowest and highest notes: a
    // steady regime safely above the reed's threshold.
    double pressureLow;
    double pressureHigh;
    // The partials the dynamic lets through: a low-pass at f0 x (open + bloom x CC1^2).
    double open;
    double bloom;
    double stretch; // cents sharp per kHz^2, taking back the loop's flattening at the top
    double level;   // output scale
};

// A reed instrument, played by MonoWind: a reed valve (J. O. Smith's reed table) on a bore. See
// docs/oboe-and-clarinet.md.
class ReedEngine : public MonoWind {
public:
    explicit ReedEngine(const ReedModel& model);
    void prepare(double sampleRate);
    void reset();
    void render(float* left, float* right, std::uint32_t frameCount);

private:
    static constexpr std::size_t delaySize = 4096;

    void silence() override;

    ReedModel model_;
    std::array<double, delaySize> bore_{};
    std::array<double, delaySize> apex_{}; // a conical bore's short side, reed to tip and back
    std::size_t write_ = 0;
    double reflection_ = 0.0; // the bell's low-pass state
    double radiated_ = 0.0;   // the tone holes' high-pass low band
    std::array<double, 4> formant_{};
    std::array<double, 2> dynamic_{}; // the dynamic's low-pass state
    double noiseLow_ = 0.0;
    std::uint32_t noise_ = 0x2545F491u;
};

struct OboeEngine : ReedEngine {
    OboeEngine();
};

struct ClarinetEngine : ReedEngine {
    ClarinetEngine();
};

} // namespace pianocraft
