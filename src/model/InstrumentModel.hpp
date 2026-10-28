#pragma once

#include <array>
#include <cstddef>

namespace pianocraft {

inline constexpr std::size_t instrumentKeyCount = 88;
inline constexpr std::size_t primaryModeLimit = 112;
inline constexpr std::size_t secondaryModeLimit = 32;
inline constexpr std::size_t modeLimit = primaryModeLimit + secondaryModeLimit;

struct ModeCoefficients {
    double sustainA1 = 0.0;
    double sustainA2 = 0.0;
    double releaseA1 = 0.0;
    double releaseA2 = 0.0;
    double excitation = 0.0;
    double output = 0.0;
    double outputRight = 0.0;
    double outputRightPrevious = 0.0;
    double frequency = 0.0;
    double decaySeconds = 0.0;
    bool secondary = false;
    // Voicing-independent values; setVoicing() derives the poles, frequency, decay and
    // excitation above from them.
    double baseFrequency = 0.0;
    double beatHz = 0.0; // signed detune of a unison partner from baseFrequency
    double baseDecaySeconds = 0.0;
    double baseReleaseSeconds = 0.0;
    double baseExcitation = 0.0;
};

struct KeyModel {
    int midi = 21;
    double fundamental = 27.5;
    double inharmonicity = 0.0;
    double strikePosition = 0.12;
    double hammerStiffness = 60000.0;
    double velocityGainExponent = 1.35;
    double velocityBrightnessExponent = 0.0;
    double hammerStiffnessScale = 1.0;
    double outputGain = 1.0;
    double sympatheticDrive = 1.0;
    // Measured deviation from equal temperament relative to the piano's own A4, in octaves.
    double stretchOctaves = 0.0;
    // Butterworth high-pass at 0.7 f0 on the voice output, as b0, b1, b2, a1, a2.
    std::array<double, 5> bridgeHighpass{1.0, 0.0, 0.0, 0.0, 0.0};
    double leftGain = 0.707;
    double rightGain = 0.707;
    bool hasDamper = true;
    std::size_t primaryModeCount = 0;
    std::size_t modeCount = 0;
    std::array<ModeCoefficients, modeLimit> modes{};
};

enum class MicPosition { spacedPair = 0, coincident = 1, player = 2 };

// User voicing on top of the measured model; 1.0 everywhere is the calibrated piano.
struct Voicing {
    double sustain = 1.0;    // every string decay time
    double release = 1.0;    // damper release times
    double detune = 1.0;     // unison partner beat rates
    double aftersound = 1.0; // unison partner level: the long second-stage tail
    double masterTune = 440.0; // A4 in Hz
    double stretch = 1.0;    // 1 keeps the recorded piano's stretch, 0 is equal temperament
};

class InstrumentModel {
public:
    explicit InstrumentModel(double sampleRate);

    const KeyModel& key(int midi) const;
    // Recomputes key panning and each partial's right-channel phase (one sin/cos per mode).
    void setMicPosition(MicPosition position);
    // Recomputes every mode's pole and partner excitation, then the mic phases (which depend on
    // frequency).
    void setVoicing(const Voicing& voicing);

private:
    std::array<KeyModel, instrumentKeyCount> keys_{};
    double sampleRate_ = 48000.0;
    double measuredA4_ = 440.0;
    MicPosition micPosition_ = MicPosition::spacedPair;
};

} // namespace pianocraft
