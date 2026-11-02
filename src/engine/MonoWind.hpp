#pragma once

#include "engine/Note.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace pianocraft {

enum class WindParameterId : std::uint32_t {
    outputGain = 1,
    breathNoise = 2,
    vibratoDepth = 3,
    vibratoRate = 4,
    brightness = 5,
    legatoTime = 6,
    masterTune = 7,
};

// The player of a monophonic wind: notes, slurs, the tongue, breath (CC1, or the sounding note's
// Pressure: piano.py writes its CC1 curves again as Pressure, which Bitwig imports), vibrato and
// the expressions. Overlapping or touching notes slur without a new attack; a held note holds. The
// instrument (FluteEngine, ReedEngine) makes the sound in its render(), stepping the player with
// pace() and advance() each block and sample.
// ponytail: releasing the sounding note ends the phrase even if an earlier key is still held;
// return to it (a held-note stack) if trills need it.
class MonoWind {
public:
    static constexpr std::size_t voiceCapacity = 1;
    using EndedNote = pianocraft::EndedNote;

    MonoWind(int lowestKey, int highestKey);
    virtual ~MonoWind() = default;

    void noteOn(const NoteAddress& address, double velocity);
    void noteOff(const NoteAddress& address, double velocity);
    void choke(const NoteAddress& address);
    // CC1 breath pressure, CC11 expression trim, CC21 vibrato amount (0.5 is the Vibrato Depth
    // setting).
    void controlChange(std::int16_t channel, std::uint8_t controller, double value);
    void pitchBend(std::int16_t channel, double bend); // -1 to 1, +-2 semitones
    void setParameter(WindParameterId id, double value);
    double parameterValue(WindParameterId id) const;
    void modulate(const NoteAddress& target, WindParameterId id, double amount); // Brightness only
    void setNoteVolume(const NoteAddress& target, double gain);
    void setNotePan(const NoteAddress& target, double pan);
    void setNoteBrightness(const NoteAddress& target, double brightness); // Timbre, 0.5 neutral
    void setNotePressure(const NoteAddress& target, double pressure);     // breath, as CC1

    std::size_t takeEndedNotes(EndedNote* destination, std::size_t capacity);
    std::size_t activeVoiceCount() const;

protected:
    static constexpr std::size_t parameterCount = 8;
    // The share of the bore's wave a half-open hole loses each round trip, on top of the bell's.
    static constexpr double leakLoss = 0.3;

    // A block's rates, from the parameters.
    struct Pace {
        double smooth, glide, attack, release, vibratoStep, vibratoDepth, wander;
    };
    // One sample of the player: breath (smoothed), the tongue's envelope, how unsettled a slur leaves
    // the bore (1 while its keys move, fading over 10 ms after), the vibrato (-1 to 1 times its
    // depth), and a slur's keys: while they move (keys < 1) the bore is at two lengths, from and to
    // (periods, in samples), in the shares 1 - seat and seat, and leak (0 to 1) is how open a
    // half-open hole leaves it.
    struct Breath {
        double breath, envelope, slur, vibrato, keys, from, to, seat, leak;
    };

    Pace pace() const;
    Breath advance(const Pace& pace);
    void resetPlayer();
    // True, having written silence, if no note sounds.
    bool silent(float* left, float* right, std::uint32_t frameCount) const;
    // Ends the note once it is released and quiet.
    void finishBlock();
    double period(int key) const; // in samples, before bend
    double parameter(WindParameterId id) const { return parameters_[static_cast<std::size_t>(id)]; }
    virtual void silence() = 0; // the instrument's own state, at a choke

    double sampleRate_ = 48000.0;
    std::array<double, parameterCount> parameters_{};
    NoteAddress note_{};
    bool sounding_ = false; // a note is playing or releasing
    bool keyDown_ = false;
    double releasedAt_ = -1.0; // noteSeconds_ when the sounding note was released
    double slurSeconds_ = 1.0e9;
    double velocity_ = 0.0;
    double attackSeconds_ = 0.0;
    double envelope_ = 0.0;
    double noteSeconds_ = 0.0;
    double periodTarget_ = 100.0;
    double period_ = 100.0;
    double periodFrom_ = 100.0; // the slurred-from note's period, while the keys move
    double keys_ = 1.0;
    double seat_ = 1.0;
    double periodVia_ = 100.0;   // the fingering between, while a slur's fingers move
    double fingerSeconds_ = 0.0; // how long the slur stays there
    double tapFrom_ = 100.0;     // the bore's two lengths while a pad moves
    double tapTo_ = 100.0;
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
    double level_ = 0.0; // the instrument sets it: its output's peak, decaying
    int registerInterval_ = 12; // the instrument sets it: the clarinet overblows a twelfth
    double wander_ = 0.0; // the instrument sets it: how unsteady the player's breath is (a share of it)
    std::uint32_t frame_ = 0;

private:
    void startNote(const NoteAddress& address, double velocity, bool legato);
    void endNote(const NoteAddress& address);

    int lowestKey_;
    int highestKey_;
    std::array<EndedNote, 16> ended_{};
    std::uint32_t wanderNoise_ = 0x6C8E9CF5u;
    std::array<double, 2> wanderState_{};
    std::size_t endedCount_ = 0;
};

} // namespace pianocraft
