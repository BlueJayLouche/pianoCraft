#include "engine/MonoWind.hpp"
#include "engine/Filters.hpp"

#include <algorithm>
#include <cmath>

namespace pianocraft {

namespace {

std::size_t index(WindParameterId id) {
    return static_cast<std::size_t>(id);
}

} // namespace

MonoWind::MonoWind(int lowestKey, int highestKey) : lowestKey_(lowestKey), highestKey_(highestKey) {
    parameters_[index(WindParameterId::outputGain)] = 0.7;
    parameters_[index(WindParameterId::breathNoise)] = 1.0;
    parameters_[index(WindParameterId::vibratoDepth)] = 1.0;
    parameters_[index(WindParameterId::vibratoRate)] = 5.0;
    parameters_[index(WindParameterId::brightness)] = 1.0;
    parameters_[index(WindParameterId::legatoTime)] = 0.012;
    parameters_[index(WindParameterId::masterTune)] = 440.0;
    parameters_[index(WindParameterId::reed)] = 1.0;
}

void MonoWind::resetPlayer() {
    sounding_ = keyDown_ = false;
    releasedAt_ = -1.0;
    slurSeconds_ = 1.0e9;
    keys_ = seat_ = 1.0;
    envelope_ = level_ = 0.0;
    breath_ = breathTarget_ = 0.8;
    expression_ = expressionTarget_ = 1.0;
    vibratoAmount_ = 1.0;
    vibratoPhase_ = 0.0;
    bend_ = 0.0;
    endedCount_ = 0;
    wanderNoise_ = 0x6C8E9CF5u;
    wanderState_ = {};
}

double MonoWind::period(int key) const {
    return sampleRate_ / (parameters_[index(WindParameterId::masterTune)] * std::pow(2.0, (key - 69) / 12.0));
}

void MonoWind::endNote(const NoteAddress& address) {
    if (endedCount_ < ended_.size()) {
        ended_[endedCount_++] = {address, frame_};
    }
}

void MonoWind::startNote(const NoteAddress& address, double velocity, bool legato) {
    if (sounding_) {
        endNote(note_); // the slur or re-attack takes over the sounding note's voice
    }
    const int fromKey = note_.key;
    note_ = address;
    sounding_ = keyDown_ = true;
    releasedAt_ = -1.0;
    periodTarget_ = period(address.key);
    volume_ = volumeTarget_ = 1.0;
    pan_ = 0.5;
    timbre_ = brightnessModulation_ = 0.0;
    if (!legato) {
        // Velocity shapes only the attack: how fast the breath arrives and how hard the tongue
        // releases it.
        velocity_ = velocity;
        attackSeconds_ = attackScale_ * (0.09 - 0.06 * velocity);
        noteSeconds_ = 0.0;
        period_ = periodFrom_ = tapFrom_ = tapTo_ = periodTarget_;
        keys_ = seat_ = 1.0;
        envelope_ = 0.0; // the tongue stops the air before it releases it again
    } else {
        // The keys move: from the note sounding now (the nearer, if a slur is still under way).
        periodFrom_ = seat_ < 0.5 ? tapFrom_ : tapTo_;
        // When several fingers move they don't land together: professionals spend 13 +- 9 ms in the
        // fingering between (21 +- 11 ms with four fingers; Almeida, Chow, Smith and Wolfe, JASA
        // 2009, flute). Mostly that fingering sounds close to one of the two notes (a "safe" order);
        // otherwise some other note, too briefly to hear as a pitch. Each pair of notes has its own
        // fingering, so the same detour each time: none for about a third of steps (one finger) and
        // for octaves (the octave key); otherwise 0.5 to 2 Legato Times.
        std::uint32_t hash = static_cast<std::uint32_t>(address.key) * 0x9E3779B1u ^ static_cast<std::uint32_t>(fromKey) * 0x85EBCA77u ^
                             static_cast<std::uint32_t>(lowestKey_) * 0xC2B2AE3Du;
        hash ^= hash >> 15;
        hash *= 0x2C1B3C6Du;
        hash ^= hash >> 12;
        const auto share = [&](int bits) { return static_cast<double>((hash >> bits) & 0xFF) / 256.0; };
        const int interval = std::abs(address.key - fromKey);
        const bool oneFinger = interval == registerInterval_ || (interval <= 2 && share(0) < 0.35);
        fingerSeconds_ = oneFinger ? 0.0 : parameters_[index(WindParameterId::legatoTime)] * (0.5 + 1.5 * share(8));
        fingerSeconds_ = fingerSeconds_ < 0.0045 ? 0.0 : fingerSeconds_; // shorter than a pad: no detour
        if (share(16) < 0.7) {
            periodVia_ = (share(24) < 0.5 ? periodFrom_ : periodTarget_) * std::pow(2.0, (share(4) < 0.5 ? 0.35 : -0.35) / 12.0);
        } else {
            int step = (share(24) < 0.5 ? 1 : -1) * (share(4) < 0.5 ? 1 : 2);
            step = address.key + step == fromKey ? -step : step;
            periodVia_ = period(address.key + step);
        }
        period_ = periodTarget_;
        keys_ = seat_ = 0.0;
        slurSeconds_ = 0.0;
    }
}

void MonoWind::noteOn(const NoteAddress& address, double velocity) {
    if (address.key < lowestKey_ || address.key > highestKey_) {
        endNote(address);
        return;
    }
    // A note that starts the instant the last one ends (piano.py's slides touch their notes; hosts
    // send the note-off first) slurs too, unless it repeats the pitch: that is tongued.
    const bool joined = sounding_ && releasedAt_ >= 0.0 && noteSeconds_ - releasedAt_ < 0.005 && address.key != note_.key;
    // Only a note that is sounding can be slurred from: out of one that never spoke, the next note
    // would appear at full breath with no attack, a click.
    const bool speaking = level_ > 0.05;
    startNote(address, std::clamp(velocity, 0.0, 1.0),
              (keyDown_ || joined) && speaking && parameters_[index(WindParameterId::legatoTime)] >= 0.0);
}

void MonoWind::noteOff(const NoteAddress& address, double) {
    if (keyDown_ && matches(address, note_)) {
        keyDown_ = false;
        releasedAt_ = noteSeconds_;
    }
}

void MonoWind::choke(const NoteAddress& address) {
    if (sounding_ && matches(address, note_)) {
        endNote(note_);
        sounding_ = keyDown_ = false;
        envelope_ = 0.0;
        silence();
    }
}

void MonoWind::controlChange(std::int16_t, std::uint8_t controller, double value) {
    value = std::clamp(value, 0.0, 1.0);
    if (controller == 1) {
        breathTarget_ = value;
    } else if (controller == 11) {
        expressionTarget_ = value;
    } else if (controller == 21) {
        vibratoAmount_ = 2.0 * value;
    }
}

void MonoWind::pitchBend(std::int16_t, double bend) {
    if (std::isfinite(bend)) {
        bend_ = std::clamp(bend, -1.0, 1.0);
    }
}

void MonoWind::setParameter(WindParameterId id, double value) {
    const auto i = index(id);
    if (i != 0 && i < parameterCount && std::isfinite(value)) {
        parameters_[i] = value;
        if (id == WindParameterId::masterTune && sounding_) {
            periodTarget_ = period_ = periodFrom_ = tapFrom_ = tapTo_ = period(note_.key);
            keys_ = seat_ = 1.0;
        }
    }
}

double MonoWind::parameterValue(WindParameterId id) const {
    const auto i = index(id);
    return i < parameterCount ? parameters_[i] : 0.0;
}

void MonoWind::modulate(const NoteAddress& target, WindParameterId id, double amount) {
    const bool global = target.noteId < 0 && target.port < 0 && target.channel < 0 && target.key < 0;
    if (id == WindParameterId::brightness && std::isfinite(amount) && (global || matches(target, note_))) {
        brightnessModulation_ = amount;
    }
}

void MonoWind::setNoteVolume(const NoteAddress& target, double gain) {
    if (sounding_ && matches(target, note_) && std::isfinite(gain)) {
        volumeTarget_ = std::clamp(gain, 0.0, 4.0);
    }
}

void MonoWind::setNotePan(const NoteAddress& target, double pan) {
    if (sounding_ && matches(target, note_) && std::isfinite(pan)) {
        pan_ = std::clamp(pan, 0.0, 1.0);
    }
}

void MonoWind::setNoteBrightness(const NoteAddress& target, double brightness) {
    if (sounding_ && matches(target, note_) && std::isfinite(brightness)) {
        timbre_ = 2.0 * (std::clamp(brightness, 0.0, 1.0) - 0.5);
    }
}

void MonoWind::setNotePressure(const NoteAddress& target, double pressure) {
    if (sounding_ && matches(target, note_) && std::isfinite(pressure)) {
        breathTarget_ = std::clamp(pressure, 0.0, 1.0);
    }
}

std::size_t MonoWind::takeEndedNotes(EndedNote* destination, std::size_t capacity) {
    const std::size_t count = std::min(capacity, endedCount_);
    std::copy_n(ended_.begin(), count, destination);
    endedCount_ = 0;
    return count;
}

std::size_t MonoWind::activeVoiceCount() const {
    return sounding_ ? 1 : 0;
}

MonoWind::Pace MonoWind::pace() const {
    const double fs = sampleRate_;
    const auto& p = parameters_;
    return {1.0 - std::exp(-1.0 / (0.015 * fs)),
            1.0 - std::exp(-1.0 / (0.01 * fs)),
            1.0 - std::exp(-1.0 / (attackSeconds_ * fs)),
            1.0 - std::exp(-1.0 / (0.06 * fs)),
            2.0 * filters::pi * std::clamp(p[index(WindParameterId::vibratoRate)], 0.5, 12.0) / fs,
            std::max(p[index(WindParameterId::vibratoDepth)], 0.0) * vibratoAmount_,
            1.0 - std::exp(-2.0 * filters::pi * 6.0 / fs)};
}

MonoWind::Breath MonoWind::advance(const Pace& pace) {
    const double fs = sampleRate_;
    breath_ += pace.smooth * (breathTarget_ - breath_);
    // The player's breath is never quite steady: a slow wander under 6 Hz (measured beside the
    // recordings, as the level's unsteadiness). Seeded, so renders repeat.
    wanderNoise_ ^= wanderNoise_ << 13;
    wanderNoise_ ^= wanderNoise_ >> 17;
    wanderNoise_ ^= wanderNoise_ << 5;
    wanderState_[0] += pace.wander * (static_cast<double>(wanderNoise_) / 2147483648.0 - 1.0 - wanderState_[0]);
    wanderState_[1] += pace.wander * (wanderState_[0] - wanderState_[1]);
    const double breath = breath_ * (1.0 + wander_ * wanderState_[1]);
    expression_ += pace.glide * (expressionTarget_ - expression_);
    volume_ += pace.glide * (volumeTarget_ - volume_);
    if (keyDown_) {
        // The tongue releases a puff that settles to the held breath within ~50 ms.
        const double target = 1.0 + puff_ * velocity_ * std::exp(-noteSeconds_ / 0.05);
        envelope_ += pace.attack * (target - envelope_);
    } else {
        envelope_ -= pace.release * envelope_;
    }
    noteSeconds_ += 1.0 / fs;
    slurSeconds_ += 1.0 / fs;
    // A slur changes the bore's length outright, no glide: each pad seats in 1.4 ms or lifts in
    // 4.1 ms, acoustically (ibid.), the half-open hole losing more of the wave meanwhile. With several
    // fingers the bore goes by the fingering between (periodVia_). Then the oscillation settles.
    double leak = 0.0;
    const double unsettled = std::exp(-std::max(slurSeconds_ - fingerSeconds_ - 0.004, 0.0) / 0.01);
    if (keys_ < 1.0) {
        const auto ease = [](double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3.0 - 2.0 * x); };
        // One pad moves the pitch a step or so, and half open it sounds a pitch between (half way
        // between two lengths the bore acts as one of their mean length). A bigger change is many
        // pads at once, or a register change (overblowing, or a register vent), the oscillation
        // jumping to the bore's next mode: switched in 1 ms, so no pitch between is heard.
        const auto pad = [](double from, double to) {
            return std::abs(12.0 * std::log2(to / from)) > 2.5 ? 0.001 : to > from ? 0.0014 : 0.0041;
        };
        const bool between = slurSeconds_ < fingerSeconds_;
        tapFrom_ = between || fingerSeconds_ <= 0.0 ? periodFrom_ : periodVia_;
        tapTo_ = between ? periodVia_ : period_;
        const double moving = between ? slurSeconds_ : slurSeconds_ - fingerSeconds_;
        seat_ = ease(moving / pad(tapFrom_, tapTo_));
        leak = 4.0 * seat_ * (1.0 - seat_);
        if (!between && seat_ >= 1.0) {
            keys_ = 1.0;
            tapFrom_ = tapTo_ = period_;
        }
    }
    // Players let vibrato in after the note has spoken.
    const double vibrato = std::sin(vibratoPhase_) * pace.vibratoDepth * std::clamp((noteSeconds_ - 0.25) / 0.6, 0.0, 1.0);
    vibratoPhase_ = std::fmod(vibratoPhase_ + pace.vibratoStep, 2.0 * filters::pi);
    return {breath, envelope_, unsettled, vibrato, keys_, tapFrom_, tapTo_, seat_, leak};
}

bool MonoWind::silent(float* left, float* right, std::uint32_t frameCount) const {
    if (sounding_) {
        return false;
    }
    std::fill_n(left, frameCount, 0.0F);
    std::fill_n(right, frameCount, 0.0F);
    return true;
}

void MonoWind::finishBlock() {
    frame_ = 0;
    if (!keyDown_ && envelope_ < 1.0e-4 && level_ < 1.0e-6) {
        endNote(note_);
        sounding_ = false;
    }
}

} // namespace pianocraft
