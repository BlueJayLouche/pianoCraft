#pragma once

#include "engine/Note.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace pianocraft {

enum class FluteParameterId : std::uint32_t {
    outputGain = 1,
    breathNoise = 2,
    vibratoDepth = 3,
    vibratoRate = 4,
    brightness = 5,
    legatoTime = 6,
    masterTune = 7,
};

// A monophonic flute: a jet-drive waveguide (Fletcher; Verge) whose breath is CC1, or the sounding
// note's Pressure (piano.py writes its CC1 curves again as Pressure, which Bitwig imports). Overlapping
// notes slur without a new attack; a held note holds. See docs/harp-and-flute.md.
// ponytail: releasing the sounding note ends the phrase even if an earlier key is still held;
// return to it (a held-note stack) if trills need it.
class FluteEngine {
public:
    static constexpr std::size_t voiceCapacity = 1;
    using EndedNote = pianocraft::EndedNote;

    FluteEngine();
    void prepare(double sampleRate);
    void reset();

    void noteOn(const NoteAddress& address, double velocity);
    void noteOff(const NoteAddress& address, double velocity);
    void choke(const NoteAddress& address);
    // CC1 breath pressure, CC11 expression trim, CC21 vibrato amount (0.5 is the Vibrato Depth
    // setting).
    void controlChange(std::int16_t channel, std::uint8_t controller, double value);
    void pitchBend(std::int16_t channel, double bend); // -1 to 1, +-2 semitones
    void setParameter(FluteParameterId id, double value);
    double parameterValue(FluteParameterId id) const;
    void modulate(const NoteAddress& target, FluteParameterId id, double amount); // Brightness only
    void setNoteVolume(const NoteAddress& target, double gain);
    void setNotePan(const NoteAddress& target, double pan);
    void setNoteBrightness(const NoteAddress& target, double brightness); // Timbre, 0.5 neutral
    void setNotePressure(const NoteAddress& target, double pressure); // breath, as CC1

    std::size_t takeEndedNotes(EndedNote* destination, std::size_t capacity);
    void render(float* left, float* right, std::uint32_t frameCount);
    std::size_t activeVoiceCount() const;

private:
    static constexpr std::size_t parameterCount = 8;
    static constexpr std::size_t delaySize = 4096;

    void startNote(const NoteAddress& address, double velocity, bool legato);
    void endNote(const NoteAddress& address);
    double period(int key) const; // in samples, before bend

    double sampleRate_ = 48000.0;
    std::array<double, parameterCount> parameters_{};
    NoteAddress note_{};
    bool sounding_ = false; // a note is playing or releasing
    bool keyDown_ = false;
    double releasedAt_ = -1.0; // noteSeconds_ when the sounding note was released
    double slurSeconds_ = 1.0e9; // since the last slur
    double velocity_ = 0.0;
    double attackSeconds_ = 0.0;
    double envelope_ = 0.0;
    double noteSeconds_ = 0.0;
    double periodTarget_ = 100.0;
    double period_ = 100.0;
    double breath_ = 0.8;
    double breathTarget_ = 0.8;
    double expression_ = 1.0;
    double expressionTarget_ = 1.0;
    double vibratoAmount_ = 1.0; // CC21, relative to Vibrato Depth
    double vibratoPhase_ = 0.0;
    double bend_ = 0.0;
    double volume_ = 1.0;
    double volumeTarget_ = 1.0;
    double pan_ = 0.5;
    double timbre_ = 0.0;
    double brightnessModulation_ = 0.0;
    double level_ = 0.0;

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
    double noiseLow_ = 0.0;
    double airLow_ = 0.0;
    double airHigh_ = 0.0;
    std::uint32_t noise_ = 0x9E3779B9u;

    std::array<EndedNote, 16> ended_{};
    std::size_t endedCount_ = 0;
    std::uint32_t frame_ = 0;
};

} // namespace pianocraft
