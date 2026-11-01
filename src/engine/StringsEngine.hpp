#pragma once

#include "engine/Note.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pianocraft {

enum class StringsParameterId : std::uint32_t {
    outputGain = 1,
    section = 2, // players per note; 1 is a soloist
    mute = 3,    // con sordino
    sulTasto = 4,
    tremolo = 5,
    harmonics = 6,
    vibratoDepth = 7,
    vibratoRate = 8,
    brightness = 9,
    spread = 10, // the section's detune and timing
    stereoWidth = 11,
    masterTune = 12,
};

// A bowed string section: each note is played by Section players, each a bowed-string waveguide
// with its own detune, bow and vibrato, through a violin, viola, cello or bass body chosen by
// register. CC1 is the bow (speed and pressure), and so is a note's own Pressure, which overrides it
// for that note: piano.py writes its CC1 curves again as Pressure, which Bitwig imports. Velocity
// shapes only the attack. See
// docs/strings-and-horn.md.
class StringsEngine {
public:
    static constexpr std::size_t voiceCapacity = 32;
    using EndedNote = pianocraft::EndedNote;

    StringsEngine();
    void prepare(double sampleRate);
    void reset();

    void noteOn(const NoteAddress& address, double velocity);
    void noteOff(const NoteAddress& address, double velocity);
    void choke(const NoteAddress& address);
    // CC1 bow, CC11 expression trim, CC21 vibrato amount (0.5 is the Vibrato Depth setting),
    // CC64 holds released notes.
    void controlChange(std::int16_t channel, std::uint8_t controller, double value);
    void pitchBend(std::int16_t channel, double bend); // -1 to 1, +-2 semitones
    void setParameter(StringsParameterId id, double value);
    double parameterValue(StringsParameterId id) const;
    void modulate(const NoteAddress& target, StringsParameterId id, double amount); // Brightness only
    void setNoteVolume(const NoteAddress& target, double gain);
    void setNotePan(const NoteAddress& target, double pan);
    void setNoteBrightness(const NoteAddress& target, double brightness); // Timbre, 0.5 neutral
    void setNotePressure(const NoteAddress& target, double pressure);     // the note's bow

    std::size_t takeEndedNotes(EndedNote* destination, std::size_t capacity);
    void render(float* left, float* right, std::uint32_t frameCount);
    std::size_t activeVoiceCount() const;

private:
    static constexpr std::size_t parameterCount = 13;
    static constexpr std::size_t playerLimit = 8;
    static constexpr std::size_t familyCount = 4; // violin, viola, cello, bass
    static constexpr std::size_t bodyModeCount = 10;

    struct Player {
        std::size_t write = 0;
        double loss = 0.0;     // the bridge's one-pole state
        double detune = 1.0;   // period factor
        double onset = 0.0;    // seconds before the bow lands
        double bowScale = 1.0;
        double bowPoint = 1.0; // this player's share of the section's bow point
        double drift = 0.0;        // intonation wandering, in octaves
        double driftTarget = 0.0;
        double driftClock = 0.0;   // seconds to the next wander
        std::uint64_t driftSeed = 0;
        double arm = 0.0;          // the bow arm's wander, -1 to 1
        double armTarget = 0.0;
        double armClock = 0.0;
        std::uint64_t armSeed = 0;
        std::uint32_t rosin = 1;   // the friction's noise
        double rosinLow = 0.0;
        double corner = 0.0;       // the radiated sound's low band, for the bow's brightness
        double envelope = 0.0;
        double vibratoPhase = 0.0;
        double vibratoStep = 0.0;
        double tremoloPhase = 0.0;
        double tremoloStep = 0.0;
        double gainLeft = 0.0;
        double gainRight = 0.0;
        double seat = 0.0;     // offset within the section, -1 to 1
    };

    struct Voice {
        NoteAddress address{};
        bool active = false;
        bool keyDown = false;
        bool held = false; // released under CC64
        double velocity = 0.0;
        double time = 0.0;
        double releasedAt = -1.0;
        std::size_t family = 0;
        std::size_t players = 1;
        std::size_t mask = 0;
        double period = 100.0; // samples at no bend
        double volume = 1.0;
        double volumeTarget = 1.0;
        double pan = 0.5;
        double level = 0.0;
        double timbre = 0.0;
        double brightnessModulation = 0.0;
        double pressure = -1.0; // per-note Pressure, unset
        double bow = 0.6;       // CC1 or Pressure, smoothed
        std::array<Player, playerLimit> player{};
    };

    struct Body {
        std::array<std::array<double, 5>, bodyModeCount> coefficients{};
        std::array<double, bodyModeCount> gain{};
        std::array<std::array<double, 2 * bodyModeCount>, 2> state{}; // left, right
        std::array<double, 2> mute{}; // the mute's low-pass, left and right
    };

    void start(Voice& voice);
    void release(Voice& voice);
    void endNote(const NoteAddress& address);
    float* line(std::size_t voice, std::size_t player, std::size_t which);

    double sampleRate_ = 48000.0;
    std::array<double, parameterCount> parameters_{};
    std::array<double, parameterCount> globalModulation_{};
    std::array<Voice, voiceCapacity> voices_{};
    std::array<Body, familyCount> body_{};
    std::vector<float> pool_;   // two delay lines per player, allocated in prepare
    std::size_t lineSize_ = 0;  // a power of two
    std::array<double, 16> bend_{};
    std::array<bool, 16> sustain_{};
    double bowTarget_ = 0.6;    // CC1
    double vibratoAmount_ = 1.0; // CC21
    double expression_ = 1.0;
    double expressionTarget_ = 1.0;
    std::array<EndedNote, 2 * voiceCapacity> ended_{};
    std::size_t endedCount_ = 0;
    std::uint32_t frame_ = 0;
};

} // namespace pianocraft
