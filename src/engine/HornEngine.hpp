#pragma once

#include "engine/Note.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pianocraft {

enum class HornParameterId : std::uint32_t {
    outputGain = 1,
    stopped = 2,
    brightness = 3,
    stereoWidth = 4,
    masterTune = 5,
};

// A horn section: each note is a lip-reed waveguide (a lip valve on a bore of one period), its
// bell and the player's hand shaping what radiates. CC1 is breath, and so is a note's own Pressure,
// which overrides it for that note (piano.py writes its CC1 curves again as Pressure, which Bitwig
// imports). Velocity shapes only the attack. See docs/strings-and-horn.md.
class HornEngine {
public:
    static constexpr std::size_t voiceCapacity = 16;
    using EndedNote = pianocraft::EndedNote;

    HornEngine();
    void prepare(double sampleRate);
    void reset();

    void noteOn(const NoteAddress& address, double velocity);
    void noteOff(const NoteAddress& address, double velocity);
    void choke(const NoteAddress& address);
    // CC1 breath, CC11 expression trim, CC64 holds released notes.
    void controlChange(std::int16_t channel, std::uint8_t controller, double value);
    void pitchBend(std::int16_t channel, double bend); // -1 to 1, +-2 semitones
    void setParameter(HornParameterId id, double value);
    double parameterValue(HornParameterId id) const;
    void modulate(const NoteAddress& target, HornParameterId id, double amount); // Brightness only
    void setNoteVolume(const NoteAddress& target, double gain);
    void setNotePan(const NoteAddress& target, double pan);
    void setNoteBrightness(const NoteAddress& target, double brightness); // Timbre, 0.5 neutral
    void setNotePressure(const NoteAddress& target, double pressure); // the note's breath

    std::size_t takeEndedNotes(EndedNote* destination, std::size_t capacity);
    void render(float* left, float* right, std::uint32_t frameCount);
    std::size_t activeVoiceCount() const;

private:
    static constexpr std::size_t parameterCount = 6;

    struct Voice {
        NoteAddress address{};
        bool active = false;
        bool keyDown = false;
        bool held = false;
        double velocity = 0.0;
        double time = 0.0;
        double releasedAt = -1.0;
        double period = 100.0;
        double envelope = 0.0;
        std::size_t write = 0;
        std::size_t mask = 0;
        double bell = 0.0;                // the bell's reflection low-pass
        double dcInput = 0.0, dcOutput = 0.0;
        std::array<double, 2> lip{};      // the lip resonance's state
        std::array<double, 2> radiation{}; // the bell's high-pass, two one-poles
        std::array<double, 2> hand{};      // the hand's low-pass, two one-poles
        std::array<double, 4> stopped{};   // the stopped hand's resonance, biquad state
        double volume = 1.0, volumeTarget = 1.0, pan = 0.5, level = 0.0;
        double timbre = 0.0, brightnessModulation = 0.0;
        double pressure = -1.0; // per-note Pressure, unset
        double breath = 0.6;    // CC1 or Pressure, smoothed
    };

    void endNote(const NoteAddress& address);

    double sampleRate_ = 48000.0;
    std::array<double, parameterCount> parameters_{};
    std::array<double, parameterCount> globalModulation_{};
    std::array<Voice, voiceCapacity> voices_{};
    std::vector<float> pool_; // one bore per voice, allocated in prepare
    std::size_t lineSize_ = 0;
    std::array<double, 16> bend_{};
    std::array<bool, 16> sustain_{};
    double breathTarget_ = 0.6; // CC1
    double expression_ = 1.0;
    double expressionTarget_ = 1.0;
    std::array<EndedNote, 2 * voiceCapacity> ended_{};
    std::size_t endedCount_ = 0;
    std::uint32_t frame_ = 0;
};

} // namespace pianocraft
