#include "engine/BrassEngine.hpp"
#include "engine/Filters.hpp"

#include <algorithm>
#include <cmath>

namespace pianocraft {

namespace {

using namespace filters;

// Fitted to the University of Iowa recordings (docs/brass.md, tools/iowa_calibration.py); the mutes
// and the stopped horn by ear. The F horn sounds B-flat 1 to F5, the B-flat trumpet E3 to C6, the
// tenor trombone E2 to F5, the tuba D1 to F4.
constexpr BrassModel horn{34, 77, 2500.0, 448.0, 3, 850.0, 3.17, -0.22, 2.5, 0.5, 1.2, {2800.0, 3.0, 2.0}, true, 1.2, 0.0, 1.0};
constexpr BrassModel trumpet{52, 84, 5000.0, 1722.0, 3, 1438.0, 0.0, 0.0, 4.0, 0.3, 2.0, {3500.0, 1.5, 1.5}, false, 1.0, 0.0, 7.0};
constexpr BrassModel trombone{40, 77, 3500.0, 967.0, 2, 1171.0, 3.22, 0.0, 5.0, 0.3, 2.0, {2000.0, 1.5, 1.5}, false, 1.0, 0.5, 0.8};
constexpr BrassModel tuba{26, 65, 1500.0, 483.0, 3, 416.0, 3.01, -1.0, 0.0, 0.5, 1.5, {1000.0, 1.5, 1.5}, false, 1.0, 0.3, 2.7};

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
// The breath's turbulence and wander, as shares of the mouth's pressure (see render).
constexpr double breathNoise = 0.07;
constexpr double breathWander = 2.5;

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

BrassEngine::BrassEngine(const BrassModel& model) : model_(model) {
    parameters_[index(HornParameterId::outputGain)] = 0.7;
    parameters_[index(HornParameterId::stopped)] = 0.0;
    parameters_[index(HornParameterId::brightness)] = 1.0;
    parameters_[index(HornParameterId::stereoWidth)] = 1.0;
    parameters_[index(HornParameterId::masterTune)] = 440.0;
}

HornEngine::HornEngine() : BrassEngine(horn) {}

TrumpetEngine::TrumpetEngine() : BrassEngine(trumpet) {}

TromboneEngine::TromboneEngine() : BrassEngine(trombone) {}

TubaEngine::TubaEngine() : BrassEngine(tuba) {}

void BrassEngine::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    lineSize_ = 1;
    // The lowest note's bore, bent down a whole tone, with room to spare.
    while (lineSize_ < static_cast<std::size_t>(1.3 * sampleRate / (440.0 * std::pow(2.0, (model_.lowestKey - 69) / 12.0))) + 16) {
        lineSize_ *= 2;
    }
    pool_.assign(voiceCapacity * lineSize_, 0.0F);
    reset();
}

void BrassEngine::reset() {
    for (auto& voice : voices_) {
        voice = Voice{};
    }
    bend_ = {};
    sustain_ = {};
    breathTarget_ = 0.6;
    expression_ = expressionTarget_ = 1.0;
    endedCount_ = 0;
}

void BrassEngine::endNote(const NoteAddress& address) {
    if (endedCount_ < ended_.size()) {
        ended_[endedCount_++] = {address, frame_};
    }
}

void BrassEngine::noteOn(const NoteAddress& address, double velocity) {
    if (address.key < model_.lowestKey || address.key > model_.highestKey) {
        endNote(address);
        return;
    }
    std::size_t stack = 0; // this key's notes already held: a stacked voice is another player
    for (const auto& other : voices_) {
        stack += other.active && other.keyDown && other.address.key == address.key ? 1 : 0;
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
    // Each note its own player, a cent or two from the others. A stacked voice is another player
    // again: 3-8 cents from the first (sharp and flat in turn) and wandering a couple of cents, so the
    // two never lock into a slow cancelling beat; coming in up to 20 ms after the first, sitting a
    // little apart, its own breath.
    const auto player = static_cast<std::uint64_t>(address.key) + 4096u * stack;
    voice->stack = stack;
    voice->noise = (0x9E3779B9u ^ (static_cast<std::uint32_t>(address.key) * 2654435761u) ^ (static_cast<std::uint32_t>(stack) * 0x85EBCA6Bu)) | 1u;
    voice->wait = stack == 0 ? 0.0 : 0.02 * unit(player + 1);
    voice->seat = stack == 0 ? 0.0 : 2.0 * unit(player + 2) - 1.0;
    const double apart = stack == 0 ? 0.0 : (stack % 2 == 1 ? 1.0 : -1.0) * (3.0 + 5.0 * unit(player + 3));
    voice->driftCents = stack == 0 ? 0.0 : 1.5 + unit(player + 4);
    voice->driftStep = 2.0 * filters::pi * (0.15 + 0.25 * unit(player + 5)) / sampleRate_;
    voice->driftPhase = 2.0 * filters::pi * unit(player + 6);
    const double cents = 4.0 * (unit(static_cast<std::uint64_t>(address.key)) - 0.5) + apart;
    voice->period = sampleRate_ / (parameters_[index(HornParameterId::masterTune)] * std::pow(2.0, (address.key - 69) / 12.0)) *
                    std::pow(2.0, -cents / 1200.0) * lipPull;
    std::size_t size = 1;
    while (size < static_cast<std::size_t>(voice->period * 1.25) + 16) {
        size *= 2;
    }
    voice->mask = std::min(size, lineSize_) - 1;
    float* bore = pool_.data() + static_cast<std::size_t>(voice - voices_.data()) * lineSize_;
    std::fill_n(bore, voice->mask + 1, 0.0F);
    if (voice->wait <= 0.0) {
        launch(*voice);
    }
}

void BrassEngine::launch(Voice& voice) {
    // The tongue's release launches the note's wave at once: the bore starts with a sinusoid of the
    // note's period circulating, which the lips take up within a period or two, rather than their
    // buzz growing from nothing (about 1 dB a millisecond, too slow beside a player's attack).
    float* bore = pool_.data() + static_cast<std::size_t>(&voice - voices_.data()) * lineSize_;
    const double seed = model_.tongue * (0.3 + 0.7 * voice.velocity) * (0.35 + 0.65 * voice.breath);
    for (std::size_t n = 1; seed > 0.0 && n <= voice.mask; ++n) {
        bore[(voice.write + voice.mask + 1 - n) & voice.mask] = static_cast<float>(-seed * std::sin(2.0 * filters::pi * static_cast<double>(n) / voice.period));
    }
}

void BrassEngine::noteOff(const NoteAddress& address, double) {
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

void BrassEngine::choke(const NoteAddress& address) {
    for (auto& voice : voices_) {
        if (voice.active && matches(address, voice.address)) {
            endNote(voice.address);
            voice = Voice{};
        }
    }
}

void BrassEngine::controlChange(std::int16_t channel, std::uint8_t controller, double value) {
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

void BrassEngine::pitchBend(std::int16_t channel, double bend) {
    if (channel >= 0 && channel < 16 && std::isfinite(bend)) {
        bend_[static_cast<std::size_t>(channel)] = std::clamp(bend, -1.0, 1.0);
    }
}

void BrassEngine::setParameter(HornParameterId id, double value) {
    const auto i = index(id);
    if (i != 0 && i < parameterCount && std::isfinite(value)) {
        parameters_[i] = value;
    }
}

double BrassEngine::parameterValue(HornParameterId id) const {
    const auto i = index(id);
    return i < parameterCount ? parameters_[i] : 0.0;
}

void BrassEngine::modulate(const NoteAddress& target, HornParameterId id, double amount) {
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

void BrassEngine::setNoteVolume(const NoteAddress& target, double gain) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(gain)) {
            voice.volumeTarget = std::clamp(gain, 0.0, 4.0);
        }
    }
}

void BrassEngine::setNotePan(const NoteAddress& target, double pan) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(pan)) {
            voice.pan = std::clamp(pan, 0.0, 1.0);
        }
    }
}

void BrassEngine::setNoteBrightness(const NoteAddress& target, double brightness) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(brightness)) {
            voice.timbre = 2.0 * (std::clamp(brightness, 0.0, 1.0) - 0.5);
        }
    }
}

void BrassEngine::setNotePressure(const NoteAddress& target, double pressure) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(pressure)) {
            voice.pressure = std::clamp(pressure, 0.0, 1.0);
        }
    }
}

std::size_t BrassEngine::takeEndedNotes(EndedNote* destination, std::size_t capacity) {
    const std::size_t count = std::min(capacity, endedCount_);
    std::copy_n(ended_.begin(), count, destination);
    endedCount_ = 0;
    return count;
}

std::size_t BrassEngine::activeVoiceCount() const {
    return static_cast<std::size_t>(std::count_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.active; }));
}

void BrassEngine::render(float* left, float* right, std::uint32_t frameCount) {
    const auto& p = parameters_;
    const double fs = sampleRate_;
    const double width = std::clamp(p[index(HornParameterId::stereoWidth)], 0.0, 1.0);
    const bool stopped = p[index(HornParameterId::stopped)] >= 0.5;
    const double output = p[index(HornParameterId::outputGain)];
    const double smooth = 1.0 - std::exp(-1.0 / (0.02 * fs));
    const double glide = 1.0 - std::exp(-1.0 / (0.01 * fs));
    const double releaseRate = 1.0 - std::exp(-1.0 / (0.06 * fs));
    const double turbulenceRate = 1.0 - std::exp(-2.0 * filters::pi * 2000.0 / fs);
    const double wanderRate = 1.0 - std::exp(-2.0 * filters::pi * 6.0 / fs);
    const double dcPole = std::pow(0.9995, 48000.0 / fs);
    const double bellPole = std::exp(-2.0 * filters::pi * model_.bell / fs);
    // The bell radiates its highs and reflects its lows: 6 dB/octave below about 500 Hz.
    const double radiationRate = 1.0 - std::exp(-2.0 * filters::pi * model_.radiation / fs);
    const auto radiationSection = butterworth(true, model_.radiation, fs, model_.poles == 3 ? 1.0 : 0.7071067811865476);
    // A stopped horn: the hand closes the bell, so what escapes is thin, nasal and metallic.
    const auto stoppedResonance = bandpass(model_.closed[0], model_.closed[1], fs);

    // Low notes left, high right, as the players' bells spread across the section.
    const int centre = (model_.lowestKey + model_.highestKey + 1) / 2;
    const double span = (model_.highestKey - model_.lowestKey + 1) / 2;
    struct Block {
        double delay, attack, hand, gainLeft, gainRight;
        std::array<double, 5> lip;
        double blare;
    };
    std::array<Block, voiceCapacity> block{};
    for (std::size_t v = 0; v < voiceCapacity; ++v) {
        auto& voice = voices_[v];
        if (!voice.active) {
            continue;
        }
        const double bend = std::pow(2.0, 2.0 * bend_[static_cast<std::size_t>(std::clamp<int>(voice.address.channel, 0, 15))] / 12.0);
        const double period = voice.period / bend * std::pow(2.0, -voice.driftCents * std::sin(voice.driftPhase) / 1200.0);
        voice.driftPhase = std::fmod(voice.driftPhase + voice.driftStep * frameCount, 2.0 * filters::pi);
        const double omega = 2.0 * filters::pi / period;
        const double loop = phaseDelay(1.0 - bellPole, 0.0, bellPole, omega) + phaseDelay(1.0, -1.0, dcPole, omega);
        const double brightness = std::clamp(p[index(HornParameterId::brightness)] + globalModulation_[index(HornParameterId::brightness)] +
                                                 voice.brightnessModulation + voice.timbre, 0.25, 2.0);
        // The hand in the bell darkens the horn: a low-pass, opening with breath.
        const double handCorner = std::min(model_.tone * brightness * (1.0 + model_.bloom * voice.breath * voice.breath * voice.breath) *
                                               std::pow(fs / period / 130.81, model_.track), 0.4 * fs);
        const double place = 0.5 + 0.25 * width * std::clamp((voice.address.key - centre) / span, -1.0, 1.0) + 0.1 * width * voice.seat + (voice.pan - 0.5);
        block[v] = {std::clamp(period - loop, 4.0, static_cast<double>(voice.mask) - 4.0),
                    1.0 - std::exp(-1.0 / (model_.attack * (0.06 - 0.045 * voice.velocity) * fs)),
                    1.0 - std::exp(-2.0 * filters::pi * handCorner / fs), std::cos(std::clamp(place, 0.0, 1.0) * 0.5 * filters::pi),
                    std::cos(std::clamp(1.0 - place, 0.0, 1.0) * 0.5 * filters::pi),
                    resonance(lipTune * lipPull * fs / period, lipQ, fs),
                    model_.blare * voice.breath * voice.breath * fs / 48000.0};
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
            if (voice.wait > 0.0) { // a stacked player not yet in
                if (!gate) {
                    endNote(voice.address);
                    continue;
                }
                voice.wait -= 1.0 / fs;
                if (voice.wait > 0.0) {
                    continue;
                }
                launch(voice);
            }
            // CC1 is the breath behind the lips, or the note's own Pressure when it has one;
            // stopped notes are blown harder.
            voice.breath += smooth * ((voice.pressure >= 0.0 ? voice.pressure : breathTarget_) - voice.breath);
            const double blowing = (0.35 + 0.65 * voice.breath) * (stopped ? model_.push : 1.0);
            // The tongue releases a puff that settles within ~60 ms.
            const double target = gate ? 1.0 + 0.4 * voice.velocity * std::exp(-voice.time / 0.06) : 0.0;
            voice.envelope += (gate ? b.attack : releaseRate) * (target - voice.envelope);
            // The breath is turbulent (noise below ~2 kHz, a few percent) and never quite steady (a
            // slow wander under 10 Hz): measured beside the recordings, the noise between the
            // harmonics and the level's unsteadiness.
            voice.noise ^= voice.noise << 13;
            voice.noise ^= voice.noise >> 17;
            voice.noise ^= voice.noise << 5;
            const double white = static_cast<double>(voice.noise) / 2147483648.0 - 1.0;
            voice.turbulence += turbulenceRate * (white - voice.turbulence);
            voice.wander[0] += wanderRate * (white - voice.wander[0]);
            voice.wander[1] += wanderRate * (voice.wander[0] - voice.wander[1]);
            const double mouth = voice.envelope * blowing * (1.0 + breathNoise * voice.turbulence + breathWander * voice.wander[1]);

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

            // At loud dynamics the crests of the outgoing wave run ahead of its troughs along the
            // bore and steepen towards shocks (Hirschberg et al., JASA 99, 1996): read the wave
            // the bell radiates at a delay that shortens with its pressure.
            const double outgoing = model_.blare > 0.0 ? lagrange(bore, voice.mask, voice.write, b.delay - b.blare * std::clamp(boreOut, -1.0, 1.0)) : boreOut;
            // The bell radiates its highs and reflects its lows: a Butterworth high-pass.
            double radiated = outgoing;
            if (model_.poles != 2) {
                voice.radiation[0] += radiationRate * (radiated - voice.radiation[0]);
                radiated -= voice.radiation[0];
            }
            if (model_.poles > 1) {
                radiated = biquad(radiationSection, radiated, voice.radiation[1], voice.radiation[2]);
            }
            // The horn's tone is its hand, so a stopped horn has none; a mute sits in a bell that
            // keeps its own.
            if (!(stopped && model_.hand)) {
                voice.hand[0] += b.hand * (radiated - voice.hand[0]);
                voice.hand[1] += b.hand * (voice.hand[0] - voice.hand[1]);
                voice.hand[2] += b.hand * (voice.hand[1] - voice.hand[2]);
                radiated = voice.hand[2];
            }
            if (stopped) {
                radiated = 0.7 * (0.3 * radiated + model_.closed[2] * biquad(stoppedResonance, radiated, voice.stopped[0], voice.stopped[1]));
            }
            voice.volume += glide * (voice.volumeTarget - voice.volume);
            radiated *= voice.volume * (model_.swell != 0.0 ? std::pow(std::max(voice.breath, 0.02) / 0.6, model_.swell) : 1.0);
            sumLeft += radiated * b.gainLeft;
            sumRight += radiated * b.gainRight;
            voice.time += 1.0 / fs;
            voice.level = std::max(std::abs(radiated), voice.level * 0.9995);
            if (!gate && voice.releasedAt >= 0.0 && voice.time - voice.releasedAt > 0.1 && voice.level < 1.0e-5) {
                endNote(voice.address);
                voice = Voice{};
            }
        }
        const double gain = 0.6 * model_.level * output * expression_;
        left[frame] = static_cast<float>(gain * sumLeft);
        right[frame] = static_cast<float>(gain * sumRight);
    }
    frame_ = 0;
}

} // namespace pianocraft
