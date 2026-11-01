#include "engine/HornEngine.hpp"
#include "engine/Filters.hpp"

#include <algorithm>
#include <cmath>

namespace pianocraft {

namespace {

using namespace filters;
constexpr int lowestKey = 34;  // B-flat 1, the F horn's lowest usual note
constexpr int highestKey = 77; // F5

// The lip valve (Cook's waveguide brass, TBone, ICMC 1991): the lips are a resonance driven by
// the pressure across them, pushed partly open by the breath (so it passes DC); their opening, the
// square of their swing, lets mouth pressure into the bore, and while they are shut the bore
// reflects. Constants set by ear and by the tests.
constexpr double lipQ = 8.0;
constexpr double lipGain = 1.0;
constexpr double reflection = 0.9;
constexpr double lipTune = 0.8; // brass lips sound above their resonance (an outward-striking valve)
// Lips tuned below the note pull the loop 17 cents sharp (a little more with breath); taken back.
const double lipPull = std::pow(2.0, 17.0 / 1200.0);

std::size_t index(HornParameterId id) {
    return static_cast<std::size_t>(id);
}

double unit(std::uint64_t x) { // splitmix64 to [0, 1)
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return static_cast<double>((x ^ (x >> 31)) >> 11) * 0x1.0p-53;
}

} // namespace

HornEngine::HornEngine() {
    parameters_[index(HornParameterId::outputGain)] = 0.7;
    parameters_[index(HornParameterId::stopped)] = 0.0;
    parameters_[index(HornParameterId::brightness)] = 1.0;
    parameters_[index(HornParameterId::stereoWidth)] = 1.0;
    parameters_[index(HornParameterId::masterTune)] = 440.0;
}

void HornEngine::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    lineSize_ = 1;
    while (lineSize_ < static_cast<std::size_t>(sampleRate / 40.0) + 16) {
        lineSize_ *= 2;
    }
    pool_.assign(voiceCapacity * lineSize_, 0.0F);
    reset();
}

void HornEngine::reset() {
    for (auto& voice : voices_) {
        voice = Voice{};
    }
    bend_ = {};
    sustain_ = {};
    breathTarget_ = 0.6;
    expression_ = expressionTarget_ = 1.0;
    endedCount_ = 0;
}

void HornEngine::endNote(const NoteAddress& address) {
    if (endedCount_ < ended_.size()) {
        ended_[endedCount_++] = {address, frame_};
    }
}

void HornEngine::noteOn(const NoteAddress& address, double velocity) {
    if (address.key < lowestKey || address.key > highestKey) {
        endNote(address);
        return;
    }
    Voice* voice = nullptr;
    for (auto& candidate : voices_) {
        if (!candidate.active) {
            voice = &candidate;
            break;
        }
    }
    if (voice == nullptr) {
        voice = &voices_[0];
        for (auto& candidate : voices_) {
            const bool better = (!candidate.keyDown && voice->keyDown) ||
                                (candidate.keyDown == voice->keyDown && candidate.level < voice->level);
            voice = better ? &candidate : voice;
        }
        endNote(voice->address);
    }
    *voice = Voice{};
    voice->address = address;
    voice->active = voice->keyDown = true;
    voice->velocity = std::clamp(velocity, 0.0, 1.0);
    voice->breath = breathTarget_;
    // Each note its own player, a cent or two from the others.
    const double cents = 4.0 * (unit(static_cast<std::uint64_t>(address.key)) - 0.5);
    voice->period = sampleRate_ / (parameters_[index(HornParameterId::masterTune)] * std::pow(2.0, (address.key - 69) / 12.0)) *
                    std::pow(2.0, -cents / 1200.0) * lipPull;
    std::size_t size = 1;
    while (size < static_cast<std::size_t>(voice->period * 1.25) + 16) {
        size *= 2;
    }
    voice->mask = std::min(size, lineSize_) - 1;
    std::fill_n(pool_.data() + static_cast<std::size_t>(voice - voices_.data()) * lineSize_, voice->mask + 1, 0.0F);
}

void HornEngine::noteOff(const NoteAddress& address, double) {
    for (auto& voice : voices_) {
        if (voice.active && voice.keyDown && matches(address, voice.address)) {
            voice.keyDown = false;
            voice.held = sustain_[static_cast<std::size_t>(std::clamp<int>(voice.address.channel, 0, 15))];
            if (!voice.held) {
                voice.releasedAt = voice.time;
            }
        }
    }
}

void HornEngine::choke(const NoteAddress& address) {
    for (auto& voice : voices_) {
        if (voice.active && matches(address, voice.address)) {
            endNote(voice.address);
            voice = Voice{};
        }
    }
}

void HornEngine::controlChange(std::int16_t channel, std::uint8_t controller, double value) {
    value = std::clamp(value, 0.0, 1.0);
    if (controller == 1) {
        breathTarget_ = value;
    } else if (controller == 11) {
        expressionTarget_ = value;
    } else if (controller == 64 && channel >= 0 && channel < 16) {
        sustain_[static_cast<std::size_t>(channel)] = value >= 0.5;
        for (auto& voice : voices_) {
            if (value < 0.5 && voice.active && voice.held && voice.address.channel == channel) {
                voice.held = false;
                voice.releasedAt = voice.time;
            }
        }
    }
}

void HornEngine::pitchBend(std::int16_t channel, double bend) {
    if (channel >= 0 && channel < 16 && std::isfinite(bend)) {
        bend_[static_cast<std::size_t>(channel)] = std::clamp(bend, -1.0, 1.0);
    }
}

void HornEngine::setParameter(HornParameterId id, double value) {
    const auto i = index(id);
    if (i != 0 && i < parameterCount && std::isfinite(value)) {
        parameters_[i] = value;
    }
}

double HornEngine::parameterValue(HornParameterId id) const {
    const auto i = index(id);
    return i < parameterCount ? parameters_[i] : 0.0;
}

void HornEngine::modulate(const NoteAddress& target, HornParameterId id, double amount) {
    if (id != HornParameterId::brightness || !std::isfinite(amount)) {
        return;
    }
    if (target.noteId < 0 && target.port < 0 && target.channel < 0 && target.key < 0) {
        globalModulation_[index(id)] = amount;
        return;
    }
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address)) {
            voice.brightnessModulation = amount;
        }
    }
}

void HornEngine::setNoteVolume(const NoteAddress& target, double gain) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(gain)) {
            voice.volumeTarget = std::clamp(gain, 0.0, 4.0);
        }
    }
}

void HornEngine::setNotePan(const NoteAddress& target, double pan) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(pan)) {
            voice.pan = std::clamp(pan, 0.0, 1.0);
        }
    }
}

void HornEngine::setNoteBrightness(const NoteAddress& target, double brightness) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(brightness)) {
            voice.timbre = 2.0 * (std::clamp(brightness, 0.0, 1.0) - 0.5);
        }
    }
}

void HornEngine::setNotePressure(const NoteAddress& target, double pressure) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(pressure)) {
            voice.pressure = std::clamp(pressure, 0.0, 1.0);
        }
    }
}

std::size_t HornEngine::takeEndedNotes(EndedNote* destination, std::size_t capacity) {
    const std::size_t count = std::min(capacity, endedCount_);
    std::copy_n(ended_.begin(), count, destination);
    endedCount_ = 0;
    return count;
}

std::size_t HornEngine::activeVoiceCount() const {
    return static_cast<std::size_t>(std::count_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.active; }));
}

void HornEngine::render(float* left, float* right, std::uint32_t frameCount) {
    const auto& p = parameters_;
    const double fs = sampleRate_;
    const double width = std::clamp(p[index(HornParameterId::stereoWidth)], 0.0, 1.0);
    const bool stopped = p[index(HornParameterId::stopped)] >= 0.5;
    const double output = p[index(HornParameterId::outputGain)];
    const double smooth = 1.0 - std::exp(-1.0 / (0.02 * fs));
    const double glide = 1.0 - std::exp(-1.0 / (0.01 * fs));
    const double releaseRate = 1.0 - std::exp(-1.0 / (0.06 * fs));
    const double dcPole = std::pow(0.9995, 48000.0 / fs);
    const double bellPole = std::exp(-2.0 * filters::pi * 2500.0 / fs);
    // The bell radiates its highs and reflects its lows: 6 dB/octave below about 500 Hz.
    const double radiationRate = 1.0 - std::exp(-2.0 * filters::pi * 500.0 / fs);
    // A stopped horn: the hand closes the bell, so what escapes is thin, nasal and metallic.
    const auto stoppedResonance = bandpass(2800.0, 3.0, fs);

    struct Block {
        double delay, attack, hand, gainLeft, gainRight;
        std::array<double, 5> lip;
    };
    std::array<Block, voiceCapacity> block{};
    for (std::size_t v = 0; v < voiceCapacity; ++v) {
        auto& voice = voices_[v];
        if (!voice.active) {
            continue;
        }
        const double bend = std::pow(2.0, 2.0 * bend_[static_cast<std::size_t>(std::clamp<int>(voice.address.channel, 0, 15))] / 12.0);
        const double period = voice.period / bend;
        const double omega = 2.0 * filters::pi / period;
        const double loop = phaseDelay(1.0 - bellPole, 0.0, bellPole, omega) + phaseDelay(1.0, -1.0, dcPole, omega);
        const double brightness = std::clamp(p[index(HornParameterId::brightness)] + globalModulation_[index(HornParameterId::brightness)] +
                                                 voice.brightnessModulation + voice.timbre, 0.25, 2.0);
        // The hand in the bell darkens the horn: a low-pass, opening with breath.
        const double handCorner = std::min(1500.0 * brightness * (0.5 + 0.8 * voice.breath), 0.4 * fs);
        const double place = 0.5 + 0.25 * width * std::clamp((voice.address.key - 56) / 22.0, -1.0, 1.0) + (voice.pan - 0.5);
        block[v] = {std::clamp(period - loop, 4.0, static_cast<double>(voice.mask) - 4.0),
                    1.0 - std::exp(-1.0 / ((0.06 - 0.045 * voice.velocity) * fs)),
                    1.0 - std::exp(-2.0 * filters::pi * handCorner / fs), std::cos(std::clamp(place, 0.0, 1.0) * 0.5 * filters::pi),
                    std::cos(std::clamp(1.0 - place, 0.0, 1.0) * 0.5 * filters::pi),
                    resonance(lipTune * lipPull * fs / period, lipQ, fs)};
    }

    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        frame_ = frame;
        expression_ += glide * (expressionTarget_ - expression_);
        double sumLeft = 0.0, sumRight = 0.0;
        for (std::size_t v = 0; v < voiceCapacity; ++v) {
            auto& voice = voices_[v];
            if (!voice.active) {
                continue;
            }
            const auto& b = block[v];
            const bool gate = voice.keyDown || voice.held;
            // CC1 is the breath behind the lips, or the note's own Pressure when it has one;
            // stopped notes are blown harder.
            voice.breath += smooth * ((voice.pressure >= 0.0 ? voice.pressure : breathTarget_) - voice.breath);
            const double blowing = (0.35 + 0.65 * voice.breath) * (stopped ? 1.2 : 1.0);
            // The tongue releases a puff that settles within ~60 ms.
            const double target = gate ? 1.0 + 0.4 * voice.velocity * std::exp(-voice.time / 0.06) : 0.0;
            voice.envelope += (gate ? b.attack : releaseRate) * (target - voice.envelope);
            const double mouth = voice.envelope * blowing;

            float* bore = pool_.data() + v * lineSize_;
            const double boreOut = lagrange(bore, voice.mask, voice.write, b.delay);
            voice.bell = (1.0 - bellPole) * boreOut + bellPole * voice.bell;
            const double reflected = reflection * voice.bell;
            const double swing = lipGain * biquad(b.lip, mouth - reflected, voice.lip[0], voice.lip[1]);
            const double open = std::max(swing, 0.0); // lips close, they do not open backwards
            const double opening = std::min(open * open, 1.0);
            const double input = opening * mouth + (1.0 - opening) * reflected;
            voice.dcOutput = input - voice.dcInput + dcPole * voice.dcOutput;
            voice.dcInput = input;
            bore[voice.write] = static_cast<float>(std::abs(voice.dcOutput) < 1.0e-30 ? 0.0 : voice.dcOutput);
            voice.write = (voice.write + 1) & voice.mask;

            voice.radiation[0] += radiationRate * (boreOut - voice.radiation[0]);
            double radiated = boreOut - voice.radiation[0];
            if (stopped) {
                radiated = 0.7 * (0.3 * radiated + 3.0 * biquad(stoppedResonance, radiated, voice.stopped[0], voice.stopped[1]));
            } else {
                voice.hand[0] += b.hand * (radiated - voice.hand[0]);
                voice.hand[1] += b.hand * (voice.hand[0] - voice.hand[1]);
                radiated = voice.hand[1];
            }
            voice.volume += glide * (voice.volumeTarget - voice.volume);
            radiated *= voice.volume;
            sumLeft += radiated * b.gainLeft;
            sumRight += radiated * b.gainRight;
            voice.time += 1.0 / fs;
            voice.level = std::max(std::abs(radiated), voice.level * 0.9995);
            if (!gate && voice.releasedAt >= 0.0 && voice.time - voice.releasedAt > 0.1 && voice.level < 1.0e-5) {
                endNote(voice.address);
                voice = Voice{};
            }
        }
        const double gain = 0.6 * output * expression_;
        left[frame] = static_cast<float>(gain * sumLeft);
        right[frame] = static_cast<float>(gain * sumRight);
    }
    frame_ = 0;
}

} // namespace pianocraft
