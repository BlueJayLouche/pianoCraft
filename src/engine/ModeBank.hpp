#pragma once

#include "model/InstrumentModel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace pianocraft {

// The strings' modes as the voice loop runs them. A piano model fills KeyModel's ModeCoefficients; the engine packs
// each key's into a ModeBank, structure-of-arrays in chunks of modeLanes modes, so the loop runs a chunk's lanes side
// by side (SIMD wherever the compiler has it) and, while a string only rings, a whole block of samples per chunk with
// its coefficients held in registers. Nothing here knows which piano filled the model.
inline constexpr std::size_t modeLanes = 4;
inline constexpr std::size_t modeChunkLimit = modeLimit / modeLanes;
static_assert(modeLimit % modeLanes == 0, "modeLimit must fill whole chunks");

struct ModeBank {
    std::size_t chunkCount = 0;
    alignas(32) std::array<double, modeLimit> sustainA1{};
    alignas(32) std::array<double, modeLimit> sustainA2{};
    alignas(32) std::array<double, modeLimit> releaseA1{};
    alignas(32) std::array<double, modeLimit> releaseA2{};
    alignas(32) std::array<double, modeLimit> residualA1{};
    alignas(32) std::array<double, modeLimit> residualA2{};
    alignas(32) std::array<double, modeLimit> outputLeft{};
    alignas(32) std::array<double, modeLimit> outputLeftPrevious{};
    alignas(32) std::array<double, modeLimit> outputRight{};
    alignas(32) std::array<double, modeLimit> outputRightPrevious{};
    alignas(32) std::array<double, modeLimit> driveCurrent{};
    alignas(32) std::array<double, modeLimit> drivePrevious{};
    alignas(32) std::array<double, modeLimit> slope{}; // primaries' only: the phantoms' drive
    alignas(32) std::array<double, modeLimit> excitation{};
    alignas(32) std::array<double, modeLimit> boost{};
    alignas(32) std::array<double, modeLimit> build{};
    alignas(32) std::array<double, modeLimit> strike{};

    // Unused lanes stay zero, so they ring at nothing and add nothing.
    void pack(const KeyModel& key) {
        chunkCount = (key.modeCount + modeLanes - 1) / modeLanes;
        for (std::size_t index = 0; index < modeLimit; ++index) {
            const bool used = index < key.modeCount;
            const ModeCoefficients none{};
            const auto& mode = used ? key.modes[index] : none;
            sustainA1[index] = mode.sustainA1;
            sustainA2[index] = mode.sustainA2;
            releaseA1[index] = mode.releaseA1;
            releaseA2[index] = mode.releaseA2;
            residualA1[index] = mode.residualA1;
            residualA2[index] = mode.residualA2;
            outputLeft[index] = mode.outputLeft;
            outputLeftPrevious[index] = mode.outputLeftPrevious;
            outputRight[index] = mode.outputRight;
            outputRightPrevious[index] = mode.outputRightPrevious;
            driveCurrent[index] = mode.driveCurrent;
            drivePrevious[index] = mode.drivePrevious;
            slope[index] = index < key.primaryModeCount ? mode.slope : 0.0;
            excitation[index] = mode.excitation;
            boost[index] = mode.boost;
            build[index] = mode.build;
            strike[index] = mode.strike;
        }
    }
};

// A voice's per-mode state, the same layout.
struct ModeStates {
    alignas(32) std::array<double, modeLimit> y1{};
    alignas(32) std::array<double, modeLimit> y2{};
};

// Per-lane sums over a key's chunks for one sample, reduced in a fixed order, so whichever path computed them, and
// however the host blocks the audio, the output is the same to the bit.
struct LaneSums {
    double left[modeLanes] = {};
    double right[modeLanes] = {};
    double drive[modeLanes] = {};
    double slope[modeLanes] = {};

    static double reduce(const double (&lanes)[modeLanes]) {
        return (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
    }
};
static_assert(modeLanes == 4, "LaneSums::reduce sums four lanes");

// A chunk's lanes unrolled, for every compiler: in the per-sample step (PianoEngine's stepModes), with constant lanes
// its per-lane sums stay in registers (rolled, the hammer's first 4 ms ran 15% slower than before the bank). The block
// kernels keep plain lane loops, which the compiler vectorizes; unrolled by hand they ran twice as slow.
template <class Lane>
inline void forEachLane(Lane&& lane) {
    lane(std::size_t{0});
    lane(std::size_t{1});
    lane(std::size_t{2});
    lane(std::size_t{3});
}

// One chunk's coefficients, copied out of the bank: a block holds them in registers while it rings.
struct ModeChunk {
    double sustainA1[modeLanes];
    double sustainA2[modeLanes];
    double releaseA1[modeLanes];
    double releaseA2[modeLanes];
    double residualA1[modeLanes];
    double residualA2[modeLanes];
    double outputLeft[modeLanes];
    double outputLeftPrevious[modeLanes];
    double outputRight[modeLanes];
    double outputRightPrevious[modeLanes];
    double driveCurrent[modeLanes];
    double drivePrevious[modeLanes];
    double slope[modeLanes];

    ModeChunk(const ModeBank& bank, std::size_t chunk) {
        const std::size_t first = chunk * modeLanes;
        for (std::size_t lane = 0; lane < modeLanes; ++lane) {
            sustainA1[lane] = bank.sustainA1[first + lane];
            sustainA2[lane] = bank.sustainA2[first + lane];
            releaseA1[lane] = bank.releaseA1[first + lane];
            releaseA2[lane] = bank.releaseA2[first + lane];
            residualA1[lane] = bank.residualA1[first + lane];
            residualA2[lane] = bank.residualA2[first + lane];
            outputLeft[lane] = bank.outputLeft[first + lane];
            outputLeftPrevious[lane] = bank.outputLeftPrevious[first + lane];
            outputRight[lane] = bank.outputRight[first + lane];
            outputRightPrevious[lane] = bank.outputRightPrevious[first + lane];
            driveCurrent[lane] = bank.driveCurrent[first + lane];
            drivePrevious[lane] = bank.drivePrevious[first + lane];
            slope[lane] = bank.slope[first + lane];
        }
    }
};

// The same coefficients read in place: for a single sample, copying them out costs more than it saves.
struct ModeChunkView {
    const double* sustainA1;
    const double* sustainA2;
    const double* releaseA1;
    const double* releaseA2;
    const double* residualA1;
    const double* residualA2;
    const double* outputLeft;
    const double* outputLeftPrevious;
    const double* outputRight;
    const double* outputRightPrevious;
    const double* driveCurrent;
    const double* drivePrevious;
    const double* slope;

    ModeChunkView(const ModeBank& bank, std::size_t chunk)
        : sustainA1(&bank.sustainA1[chunk * modeLanes]), sustainA2(&bank.sustainA2[chunk * modeLanes]),
          releaseA1(&bank.releaseA1[chunk * modeLanes]), releaseA2(&bank.releaseA2[chunk * modeLanes]),
          residualA1(&bank.residualA1[chunk * modeLanes]), residualA2(&bank.residualA2[chunk * modeLanes]),
          outputLeft(&bank.outputLeft[chunk * modeLanes]), outputLeftPrevious(&bank.outputLeftPrevious[chunk * modeLanes]),
          outputRight(&bank.outputRight[chunk * modeLanes]),
          outputRightPrevious(&bank.outputRightPrevious[chunk * modeLanes]),
          driveCurrent(&bank.driveCurrent[chunk * modeLanes]), drivePrevious(&bank.drivePrevious[chunk * modeLanes]),
          slope(&bank.slope[chunk * modeLanes]) {}
};

// One mode for one sample: its pole (blended toward the dampers' while they move or rest on the string; a blend of 0
// leaves it to the bit, so Blend is only a saving), the recursion, and what it adds to the radiated channels, the
// bridge drive and the phantoms' slope. Every path runs this.
template <bool Blend, class Chunk>
inline void ringMode(const Chunk& chunk, std::size_t lane, double input, double release, double residual,
                     double& y1, double& y2, double& a1, double& a2, double& left, double& right, double& drive,
                     double& slope) {
    a1 = chunk.sustainA1[lane];
    a2 = chunk.sustainA2[lane];
    if (Blend) {
        double releaseA1 = chunk.releaseA1[lane];
        double releaseA2 = chunk.releaseA2[lane];
        releaseA1 += (chunk.residualA1[lane] - releaseA1) * residual;
        releaseA2 += (chunk.residualA2[lane] - releaseA2) * residual;
        a1 += (releaseA1 - a1) * release;
        a2 += (releaseA2 - a2) * release;
    }
    const double output = input + a1 * y1 - a2 * y2;
    y2 = y1;
    y1 = std::abs(output) < 1.0e-30 ? 0.0 : output;
    left += y1 * chunk.outputLeft[lane] + y2 * chunk.outputLeftPrevious[lane];
    drive += y1 * chunk.driveCurrent[lane] + y2 * chunk.drivePrevious[lane];
    right += y1 * chunk.outputRight[lane] + y2 * chunk.outputRightPrevious[lane];
    slope += y1 * chunk.slope[lane];
}

// A strike's own motion in one mode for one sample, while the hammer is on it, its lifts come in or the board builds it
// (PianoEngine's stepModes): the same recursion on what this strike put in, the board's build-up advanced, and the
// share of it the radiated channels don't yet carry taken back out. Returns its prediction for the next sample, which
// the hammer meets.
template <class Chunk>
inline double strikeMode(const Chunk& chunk, std::size_t lane, double input, double a1, double a2, double build,
                         double boost, double unlifted, double& unbuilt, double& ownY1, double& ownY2, double& left,
                         double& right) {
    unbuilt *= build;
    const double moved = input + a1 * ownY1 - a2 * ownY2;
    ownY2 = ownY1;
    ownY1 = std::abs(moved) < 1.0e-30 ? 0.0 : moved;
    const double cut = 1.0 - (1.0 - boost * unlifted) * (1.0 - unbuilt);
    left -= cut * (ownY1 * chunk.outputLeft[lane] + ownY2 * chunk.outputLeftPrevious[lane]);
    right -= cut * (ownY1 * chunk.outputRight[lane] + ownY2 * chunk.outputRightPrevious[lane]);
    return a1 * ownY1 - a2 * ownY2;
}

// A ringing string for a block: no hammer, no strike of its own still building. Chunk by chunk, each over every
// sample with its state and coefficients in locals; release and residual per sample, sums per sample.
template <bool Blend>
inline void ringBlock(const ModeBank& bank, ModeStates& states, std::size_t frames, const double* release,
                      const double* residual, LaneSums* sums) {
    for (std::size_t chunk = 0; chunk < bank.chunkCount; ++chunk) {
        const ModeChunk coefficients(bank, chunk);
        const std::size_t first = chunk * modeLanes;
        double y1[modeLanes];
        double y2[modeLanes];
        for (std::size_t lane = 0; lane < modeLanes; ++lane) {
            y1[lane] = states.y1[first + lane];
            y2[lane] = states.y2[first + lane];
        }
        for (std::size_t frame = 0; frame < frames; ++frame) {
            auto& sum = sums[frame];
            for (std::size_t lane = 0; lane < modeLanes; ++lane) {
                double a1 = 0.0;
                double a2 = 0.0;
                ringMode<Blend>(coefficients, lane, 0.0, release[frame], residual[frame], y1[lane], y2[lane], a1, a2,
                                sum.left[lane], sum.right[lane], sum.drive[lane], sum.slope[lane]);
            }
        }
        for (std::size_t lane = 0; lane < modeLanes; ++lane) {
            states.y1[first + lane] = y1[lane];
            states.y2[first + lane] = y2[lane];
        }
    }
}

// A string the hammer has left whose strike is still building, for a block: as ringBlock, with that strike's own motion
// tracked per mode while it lasts (unlifted per sample). Returns the largest share still to build, so the voice knows
// when it only rings.
template <bool Blend>
inline double buildBlock(const ModeBank& bank, ModeStates& states, ModeStates& own, double* unbuiltStates,
                         std::size_t frames, const double* release, const double* residual, const double* unlifted,
                         LaneSums* sums) {
    double building = 0.0;
    for (std::size_t chunk = 0; chunk < bank.chunkCount; ++chunk) {
        const ModeChunk coefficients(bank, chunk);
        const std::size_t first = chunk * modeLanes;
        double y1[modeLanes];
        double y2[modeLanes];
        double ownY1[modeLanes];
        double ownY2[modeLanes];
        double unbuilt[modeLanes];
        double build[modeLanes];
        double boost[modeLanes];
        for (std::size_t lane = 0; lane < modeLanes; ++lane) {
            y1[lane] = states.y1[first + lane];
            y2[lane] = states.y2[first + lane];
            ownY1[lane] = own.y1[first + lane];
            ownY2[lane] = own.y2[first + lane];
            unbuilt[lane] = unbuiltStates[first + lane];
            build[lane] = bank.build[first + lane];
            boost[lane] = bank.boost[first + lane];
        }
        for (std::size_t frame = 0; frame < frames; ++frame) {
            auto& sum = sums[frame];
            for (std::size_t lane = 0; lane < modeLanes; ++lane) {
                double a1 = 0.0;
                double a2 = 0.0;
                ringMode<Blend>(coefficients, lane, 0.0, release[frame], residual[frame], y1[lane], y2[lane], a1, a2,
                                sum.left[lane], sum.right[lane], sum.drive[lane], sum.slope[lane]);
                if (unlifted[frame] > 0.0 || unbuilt[lane] > 1.0e-4) {
                    strikeMode(coefficients, lane, 0.0, a1, a2, build[lane], boost[lane], unlifted[frame],
                               unbuilt[lane], ownY1[lane], ownY2[lane], sum.left[lane], sum.right[lane]);
                }
            }
        }
        for (std::size_t lane = 0; lane < modeLanes; ++lane) {
            states.y1[first + lane] = y1[lane];
            states.y2[first + lane] = y2[lane];
            own.y1[first + lane] = ownY1[lane];
            own.y2[first + lane] = ownY2[lane];
            unbuiltStates[first + lane] = unbuilt[lane];
            building = std::max(building, unbuilt[lane]);
        }
    }
    return building;
}

} // namespace pianocraft
