#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace pianocraft {

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

    void render(float* left, float* right, std::uint32_t frameCount);
    std::size_t activeVoiceCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pianocraft
