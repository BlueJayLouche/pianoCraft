#pragma once

#include "engine/Note.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace pianocraft {

enum class StruckParameterId : std::uint32_t {
    outputGain = 1,
    brightness = 2,
    damping = 3,
    sustainLength = 4,
    stereoWidth = 5,
    masterTune = 6,
    motorSpeed = 7, // the vibraphone's: Hz, 0 stops the motor with the discs open
    motorDepth = 8,
};

// One mode of a struck bar or disc, by octaves x from the model's reference key: its frequency
// ratio to the fundamental, r0 + r1 x; its level at ff in dB re the fundamental, l0 + l1 x; and its
// T60, exp(t0 + t1 x + t2 x^2).
struct StruckMode {
    double r0, r1, l0, l1, t0, t1, t2;
};

struct StruckModel {
    int lowestKey, highestKey, referenceKey;
    std::array<StruckMode, 7> modes; // the unused end of the list has r0 0
    double fundamentalSlope; // the fundamental's ff level, dB an octave
    double dynamicRange;     // dB from pp (velocity 0.35) to ff (0.9), at 1 kHz
    double softening;        // and more, dB an octave above it: a soft stroke's longer contact
    bool keyed;              // a keyboard: every key's fundamental has the whole dynamicRange, its upper modes soften
    double reach[2];         // the octaves (from the reference) the recordings cover; laws hold their ends beyond
    double scatter[2];       // each bar or disc differs: its upper modes' levels +-dB and ratios +-%
    double beating;          // modes split into beating pairs, up to this many Hz apart (0: none)
    int beatingFrom;         // from this mode up to the 3rd
    double spread;           // how far the low and high ends sit apart in the stereo image
    double damping;          // Note-Off Damping's default: 1, the celesta's dampers; 0, crotales let ring
    double level;
};

// Struck metal: a celesta's steel bars, the crotales' bronze discs, a vibraphone's aluminium bars and a
// handpan's tone fields, struck from rest into modes measured from the University of Iowa recordings where
// they exist. See docs/celesta-and-crotales.md and docs/vibraphone-and-handpan.md.
class StruckEngine {
public:
    static constexpr std::size_t voiceCapacity = 64;
    using EndedNote = pianocraft::EndedNote;

    explicit StruckEngine(const StruckModel& model);
    void prepare(double sampleRate);
    void reset();

    void noteOn(const NoteAddress& address, double velocity);
    void noteOff(const NoteAddress& address, double velocity);
    void choke(const NoteAddress& address);
    // CC64 holds off the dampers; CC11 trims the output. Velocity is the dynamic.
    void controlChange(std::int16_t channel, std::uint8_t controller, double value);
    void pitchBend(std::int16_t channel, double bend); // -1 to 1, +-2 semitones
    void setParameter(StruckParameterId id, double value);
    double parameterValue(StruckParameterId id) const;
    // Brightness accepts per-note modulation; like the mallet, it shapes the note at the strike.
    void modulate(const NoteAddress& target, StruckParameterId id, double amount);
    void setNoteVolume(const NoteAddress& target, double gain);
    void setNotePan(const NoteAddress& target, double pan);
    void setNoteBrightness(const NoteAddress& target, double brightness); // Timbre, 0.5 neutral
    void setNotePressure(const NoteAddress&, double) {}

    std::size_t takeEndedNotes(EndedNote* destination, std::size_t capacity);
    void render(float* left, float* right, std::uint32_t frameCount);
    std::size_t activeVoiceCount() const;

private:
    static constexpr std::size_t modeLimit = 10;
    static constexpr std::size_t parameterCount = 9;

    struct Mode {
        double frequency = 0.0; // at A4 = 440 Hz and no bend
        double radius = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        double y1 = 0.0;
        double y2 = 0.0;
    };

    struct Voice {
        NoteAddress address{};
        bool active = false;
        bool keyDown = false;
        bool damped = false;
        std::array<Mode, modeLimit> modes{};
        std::size_t modeCount = 0;
        double volume = 1.0;
        double volumeTarget = 1.0;
        double pan = 0.5;
        double level = 0.0;
        std::uint64_t age = 0;
        double brightnessModulation = 0.0;
        double timbre = 0.0;
        double pendingVelocity = -1.0; // struck at the next render, so modulation sent with it applies
    };

    void strike(Voice& voice, double velocity);
    void damp(Voice& voice);
    void retune(Voice& voice);
    void endNote(const NoteAddress& address);
    double tuningFactor(std::int16_t channel) const;

    StruckModel model_;
    double sampleRate_ = 48000.0;
    std::array<double, parameterCount> parameters_{};
    std::array<double, parameterCount> globalModulation_{};
    std::array<Voice, voiceCapacity> voices_{};
    std::array<double, 16> bend_{};
    std::array<bool, 16> sustain_{};
    double expression_ = 1.0;
    double expressionTarget_ = 1.0;
    double motorPhase_ = 0.0; // one shaft turns every resonator's disc
    double motorRate_ = 0.0;
    double motorDepth_ = 0.0;
    std::array<EndedNote, 2 * voiceCapacity> ended_{};
    std::size_t endedCount_ = 0;
    std::uint32_t frame_ = 0;
};

class CelestaEngine : public StruckEngine {
public:
    CelestaEngine();
};

class CrotalesEngine : public StruckEngine {
public:
    CrotalesEngine();
};

class VibraphoneEngine : public StruckEngine {
public:
    VibraphoneEngine();
};

class HandpanEngine : public StruckEngine {
public:
    HandpanEngine();
};

} // namespace pianocraft
