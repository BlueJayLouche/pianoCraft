#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace pianocraft {

// What the model measured and fitted for one piano. Each piano's values are generated into src/model/pianos/<name>/
// (tools/fit_piano.py --piano <name>) in its own namespace, in the layout below, which every piano shares;
// src/model/Pianos.cpp gathers them into PianoData, and InstrumentModel builds from one.

inline constexpr std::size_t calibrationPartialLimit = 112;

struct PartialCalibration {
    std::uint16_t partial;
    double frequency;
    double decaySeconds;
    double secondaryFrequency;
    double secondaryDecaySeconds;
    double secondaryGain;
    bool frequencyMeasured;
    bool decayMeasured;
    bool secondaryAccepted;
};

// A key as measured: its fundamental and stiffness, level and velocity law, stereo balance and partials.
struct CalibrationData {
    double fundamental;
    double inharmonicity;
    double decaySeconds;
    double loudness;
    double hammerStiffness;
    double velocityGainExponent;
    double velocityBrightnessExponent;
    double hammerStiffnessScale;
    double stereoLeftGain;
    double stereoRightGain;
    std::uint16_t partialCount;
    std::array<PartialCalibration, calibrationPartialLimit> partials;
};

struct BodyCalibration {
    std::array<double, 3> directGain;
    std::array<double, 3> resonanceGain;
    std::array<double, 3> spectralTilt;
};

// The fitted tables' frequencies: band b is spectrumLowest x 2^(b / spectrumBandsPerOctave); partials 1-112.
inline constexpr double spectrumLowest = 25.0;
inline constexpr double spectrumBandsPerOctave = 12;
inline constexpr std::size_t spectrumBands = 110;
inline constexpr std::size_t spectrumPartials = 112;
inline constexpr std::size_t tuningPartials = 112;

using KeyCalibration = std::array<CalibrationData, 88>;
using BandTable = std::array<std::array<float, spectrumBands>, 88>;
using VelocityTable = std::array<std::array<std::array<float, spectrumBands>, 2>, 88>;
using PartialTable = std::array<std::array<float, spectrumPartials>, 88>;
using TuningTable = std::array<std::array<float, tuningPartials>, 88>;

struct PianoData {
    const char* name;
    const KeyCalibration& calibration;
    const BodyCalibration& body;
    // Fitted (tools/fit_piano.py): see SpectrumCalibration.hpp's comments in the piano's folder.
    const std::array<double, 88>& strikePositions;
    double strikeSpread;
    const BandTable& radiationDb;
    const PartialTable& partialDb;
    const VelocityTable& velocityDb;
    const VelocityTable& brighteningDb;
    const PartialTable& promptDecayLog2;
    const PartialTable& aftersoundDecayLog2;
    const PartialTable& aftersoundLevelDb;
    const TuningTable& partialCents;
    // Selected (docs/model-laws.md, the phantom row): the share of the longitudinal bridge force the board radiates,
    // on the phantom partials' level against the piano's recordings.
    double phantomCoupling;
    // Selected: each voice's output scaled so every piano plays at the Upright's level at the same settings (its ff chords
    // peak alike); the fits keep each piano's absolute level as its recordings set it (tools/render.cpp undoes this).
    double outputTrim;
};

// The pianos built in (src/model/Pianos.cpp), the first the default.
std::size_t pianoCount();
const PianoData& piano(std::size_t index);

} // namespace pianocraft
