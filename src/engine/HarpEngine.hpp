#pragma once

#include "engine/Note.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace pianocraft {

enum class HarpParameterId : std::uint32_t {
    outputGain = 1,
    pluckPosition = 2,
    presDeLaTable = 3,
    harmonics = 4,
    brightness = 5,
    damping = 6,
    sustainLength = 7,
    bodyLevel = 8,
    stereoWidth = 9,
    masterTune = 10,
    sympatheticLevel = 11,
};

// A concert harp: one string per key (C1-G7, B0 with the C-flat pedal), plucked from rest into
// modes that ring until they die away or the hand damps them. See docs/harp-and-flute.md.
class HarpEngine {
public:
    static constexpr std::size_t voiceCapacity = 96;
    using EndedNote = pianocraft::EndedNote;

    HarpEngine();
    void prepare(double sampleRate);
    void reset();

    void noteOn(const NoteAddress& address, double velocity);
    void noteOff(const NoteAddress& address, double velocity);
    void choke(const NoteAddress& address);
    // CC64 holds off the hand's damping; CC11 trims the output. Plucks take no CC1: velocity is
    // the dynamic, as in the orchestral libraries.
    void controlChange(std::int16_t channel, std::uint8_t controller, double value);
    void pitchBend(std::int16_t channel, double bend); // -1 to 1, +-2 semitones
    void setParameter(HarpParameterId id, double value);
    double parameterValue(HarpParameterId id) const;
    // Brightness and Pluck Position accept per-note modulation; like the finger, it shapes the
    // note at the pluck.
    void modulate(const NoteAddress& target, HarpParameterId id, double amount);
    void setNoteVolume(const NoteAddress& target, double gain);
    void setNotePan(const NoteAddress& target, double pan);
    void setNoteBrightness(const NoteAddress& target, double brightness); // Timbre, 0.5 neutral
    void setNotePressure(const NoteAddress&, double) {}

    std::size_t takeEndedNotes(EndedNote* destination, std::size_t capacity);
    void render(float* left, float* right, std::uint32_t frameCount);
    std::size_t activeVoiceCount() const;

private:
    static constexpr std::size_t modeLimit = 52;
    static constexpr std::size_t parameterCount = 12;
    static constexpr int lowestString = 23; // B0
    static constexpr std::size_t stringCount = 82; // to G#7
    static constexpr std::size_t sympatheticPartials = 6;
    static constexpr std::size_t bodyModeCount = 12;

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
        // The string's static load on the board before the pluck: the modes start at full swing, so
        // it is taken off as the board springs back over a period, and the output starts from rest.
        double release = 0.0;
        double releaseDecay = 0.0;
        double glideCents = 0.0;    // tension modulation: sharp at a hard pluck, settling as it dies
        double glideSamples = 1.0;
        // Per-note modulation and Timbre, applied at the pluck.
        double brightnessModulation = 0.0;
        double positionModulation = 0.0;
        double timbre = 0.0;
        // A note-on plucks at the next render, so per-note modulation sent with it applies.
        double pendingVelocity = -1.0;
        int stringKey = -1; // the string it plucked (an octave down for harmonics)
    };

    // A string the pedals sound, ringing in sympathy: driven by the soundboard, undamped.
    struct Sympathetic {
        std::array<Mode, sympatheticPartials> modes{};
        std::array<double, sympatheticPartials> drive{};
        std::array<double, sympatheticPartials> radiation{};
        std::size_t count = 0;
        double level = 0.0;
    };

    struct BodyMode {
        std::array<double, 5> coefficients{};
        std::array<double, 4> state{};
        double gain = 0.0;
    };

    void pluck(Voice& voice, double velocity);
    void damp(Voice& voice);
    void retune(Voice& voice);
    void tuneSympathy();
    void setPedal(int pitchClass);
    void endNote(const NoteAddress& address);
    double tuningFactor(std::int16_t channel) const;

    double sampleRate_ = 48000.0;
    std::array<double, parameterCount> parameters_{};
    std::array<double, parameterCount> globalModulation_{};
    std::array<Voice, voiceCapacity> voices_{};
    std::array<BodyMode, bodyModeCount> body_{};
    std::array<Sympathetic, stringCount> sympathy_{};
    std::array<bool, 12> pedals_{};            // pitch classes the pedals give the strings
    std::array<std::uint64_t, 12> pedalUsed_{};
    std::uint64_t clock_ = 0;                  // counts plucks, for the pedals' last use
    std::array<double, 16> bend_{};
    std::array<bool, 16> sustain_{};
    double expression_ = 1.0;
    double expressionTarget_ = 1.0;
    std::array<EndedNote, 2 * voiceCapacity> ended_{};
    std::size_t endedCount_ = 0;
    std::uint32_t frame_ = 0;
};

} // namespace pianocraft
