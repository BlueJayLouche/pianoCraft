#pragma once

#include <array>
#include <cstddef>

namespace pianocraft {

inline constexpr std::size_t instrumentKeyCount = 88;
inline constexpr std::size_t primaryModeLimit = 112;
inline constexpr std::size_t secondaryModeLimit = 32;
inline constexpr std::size_t modeLimit = primaryModeLimit + secondaryModeLimit;
inline constexpr std::size_t longitudinalLimit = 8;

struct ModeCoefficients {
    double sustainA1 = 0.0;
    double sustainA2 = 0.0;
    double releaseA1 = 0.0;
    double releaseA2 = 0.0;
    double excitation = 0.0;
    double output = 0.0; // radiation weight; the channels below add the mode's phase to it
    double outputLeft = 0.0;
    double outputLeftPrevious = 0.0;
    double outputRight = 0.0;
    double outputRightPrevious = 0.0;
    double frequency = 0.0;
    double decaySeconds = 0.0;
    bool secondary = false;
    // Voicing-independent values; setVoicing() derives the poles, frequency, decay and
    // excitation above from them.
    double baseFrequency = 0.0;
    double baseDecaySeconds = 0.0;
    double baseReleaseSeconds = 0.0;
    double baseExcitation = 0.0;
    double phase = 0.0; // radiated phase, set for coupled unison modes
    // Physical string displacement under the hammer (m) per unit of resonator output.
    double strike = 0.0;
    double baseStrike = 0.0;
    // Partial number times the modal displacement (m) per unit of resonator output: the string
    // slope that drives its longitudinal motion.
    double slope = 0.0;
    double baseSlope = 0.0;
};

// A longitudinal string mode (Bank & Sujbert 2005): a resonator driven by the transverse modes'
// slope products, whose bridge force carries the phantom partials.
struct LongitudinalMode {
    double a1 = 0.0;
    double a2 = 0.0;
    double input = 0.0;  // modal displacement (m) per newton for one sample
    double output = 0.0; // radiated bridge force (N) per metre
    double baseFrequency = 0.0;
    double inverseMass = 0.0;
};

// Two strings of a unison partial coupled through the bridge (Weinreich 1977): coupling
// damping + i reactance and mistuning at Unison Detune 1 in rad/s, intrinsic loss in 1/s.
struct Unison {
    std::size_t prompt = 0;
    std::size_t partner = 0;
    double loss = 0.0;
    double damping = 0.0;
    double reactance = 0.0;
    double mistuning = 0.0;
};

struct KeyModel {
    int midi = 21;
    double fundamental = 27.5;
    double inharmonicity = 0.0;
    double strikePosition = 0.12;
    // Stulov hammer: mass (kg), static felt stiffness Q0 (N/m^p), exponent p and the felt's
    // hysteresis time (s), plus the strike point's give from string modes not synthesized (m/N).
    double hammerMass = 0.008;
    double feltStiffness = 1.0e10;
    double feltExponent = 3.7;
    double feltRelaxation = 0.0;
    double residualCompliance = 0.0;
    double strikeCompliance = 0.0; // the synthesized modes' response within one sample (m/N)
    double velocityGainExponent = 1.35;
    // Age's per-key wear: a hammer hardness factor and a key response gain.
    double wearHardness = 1.0;
    double wearGain = 1.0;
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
    std::size_t unisonCount = 0;
    std::array<Unison, secondaryModeLimit> unisons{};
    // Longitudinal excitation (N) per unit of the slope products' sum.
    double longitudinalDrive = 0.0;
    std::size_t longitudinalCount = 0;
    std::array<LongitudinalMode, longitudinalLimit> longitudinal{};
};

enum class MicPosition { spacedPair = 0, coincident = 1, player = 2 };

// User voicing on top of the measured model; 1.0 everywhere is the calibrated piano.
struct Voicing {
    double sustain = 1.0;    // every string decay time
    double release = 1.0;    // damper release times
    double detune = 1.0;     // unison mistuning
    double aftersound = 1.0; // level of the unisons' slow mode: the long second-stage tail
    double masterTune = 440.0; // A4 in Hz
    double stretch = 1.0;    // 1 keeps the recorded piano's stretch, 0 is equal temperament
    double age = 0.0;        // 0 as calibrated, 1 most worn; see setVoicing
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
