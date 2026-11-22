#pragma once

#include "engine/Note.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace pianocraft {

// More slots than keys: a restrike reuses its key's voice, so even a fully pedalled keyboard
// never forces an audible voice to be cut.
inline constexpr std::size_t voiceCapacity = 96;

enum class ParameterId : std::uint32_t {
    outputGain = 1,
    hammerHardness = 2,
    sympatheticLevel = 3,
    bodyLevel = 4,
    micPosition = 5,
    stereoWidth = 6,
    dynamicRange = 7,
    velocityBrightness = 8,
    sustainLength = 9,
    damperRelease = 10,
    unisonDetune = 11,
    aftersound = 12,
    hammerNoise = 13,
    masterTune = 14,
    stretch = 15,
    age = 16,
    pedalNoise = 17,
    piano = 18, // which built-in piano (model/PianoData.hpp) new notes play
};

class PianoEngine {
public:
    static constexpr std::size_t voiceCapacity = pianocraft::voiceCapacity;
    // The plugin's level over the level the pianos are calibrated and scored at (+8 dB): at the default Output Gain a
    // velocity-127 C4 peaks near -18 dBFS and a pedalled ff chord of six notes near -5 (at the calibration level
    // they peaked at -26 and -13, very quiet beside other instruments).
    static constexpr double playbackLevel = 2.512;
    // playbackLevel and the selected piano's trim (PianoData::outputTrim): what tools/render.cpp divides out to
    // render at the calibration level.
    double calibrationGain() const;
    PianoEngine();
    ~PianoEngine();

    PianoEngine(const PianoEngine&) = delete;
    PianoEngine& operator=(const PianoEngine&) = delete;

    void prepare(double sampleRate);
    void reset();

    void noteOn(const NoteAddress& address, double velocity);
    void noteOff(const NoteAddress& address, double velocity);
    void choke(const NoteAddress& address);
    // Pedal depth 0 (up) to 1 (down); continuous values give half-pedalling and half-soft.
    void setSustain(std::int16_t channel, double depth);
    void setSoftPedal(std::int16_t channel, double depth);
    // MIDI controllers as 0-1: CC64 sustain, CC67 soft pedal. The piano ignores pitch bend.
    void controlChange(std::int16_t channel, std::uint8_t controller, double value) {
        if (controller == 64) {
            setSustain(channel, value);
        } else if (controller == 67) {
            setSoftPedal(channel, value);
        }
    }
    void pitchBend(std::int16_t, double) {}
    void setParameter(ParameterId id, double value);
    double parameterValue(ParameterId id) const;

    // CLAP per-note targeting: a negative noteId, port, channel or key matches any, and all
    // negative is global modulation. Only Hammer Hardness and Hammer Noise are modulatable.
    void modulate(const NoteAddress& target, ParameterId id, double amount);
    void setNoteVolume(const NoteAddress& target, double gain); // linear, 0-4
    void setNotePan(const NoteAddress& target, double pan);     // 0 left, 0.5 centre, 1 right
    // Bitwig's Timbre: 0.5 is neutral; 0 and 1 move Hammer Hardness by -1 and +1. Like the real
    // hammer, it shapes a note at the strike, not after.
    void setNoteBrightness(const NoteAddress& target, double brightness);
    void setNotePressure(const NoteAddress&, double) {}

    using EndedNote = pianocraft::EndedNote;
    // Notes whose voices ended (decayed, choked, stolen or restruck) since the last call, in order.
    // Frames count from the start of the latest render() call, and are 0 for a note-on.
    std::size_t takeEndedNotes(EndedNote* destination, std::size_t capacity);

    void render(float* left, float* right, std::uint32_t frameCount);
    std::size_t activeVoiceCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pianocraft
