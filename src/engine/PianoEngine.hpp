#pragma once

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
};

struct NoteAddress {
    std::int32_t noteId = -1;
    std::int16_t port = 0;
    std::int16_t channel = 0;
    std::int16_t key = 60;
};

class PianoEngine {
public:
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
    void setParameter(ParameterId id, double value);
    double parameterValue(ParameterId id) const;

    // CLAP per-note targeting: a negative noteId, port, channel or key matches any, and all
    // negative is global modulation. Only Hammer Hardness and Hammer Noise are modulatable.
    void modulate(const NoteAddress& target, ParameterId id, double amount);
    void setNoteVolume(const NoteAddress& target, double gain); // linear, 0-4
    void setNotePan(const NoteAddress& target, double pan);     // 0 left, 0.5 centre, 1 right

    struct EndedNote {
        NoteAddress address;
        std::uint32_t frame = 0;
    };
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
