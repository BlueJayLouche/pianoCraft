#include "engine/StringsEngine.hpp"
#include "engine/Filters.hpp"

#include <algorithm>
#include <cmath>

namespace pianocraft {

namespace {

using namespace filters;
constexpr int lowestKey = 28;   // E1, the double bass's lowest string
constexpr int highestKey = 100; // E7

std::size_t index(StringsParameterId id) {
    return static_cast<std::size_t>(id);
}

// The family that plays a key: basses below C2, cellos below C3, violas below G3, violins above.
std::size_t familyFor(int key) {
    return key < 36 ? 3 : key < 48 ? 2 : key < 55 ? 1 : 0;
}

// A violin's body (frequency, Q, signed gain): the signature modes A0 (air), CBR, B1- and B1+
// (Bissinger), a few higher ones, and the bridge hill near 2.5 kHz (Jansson). Analytic, set by
// ear. The larger instruments scale the frequencies by size, the bridge hill by its square root.
constexpr std::array<std::array<double, 3>, 10> violinBody{{
    {{275.0, 15.0, 0.8}}, {{405.0, 20.0, -0.4}}, {{470.0, 18.0, 1.0}}, {{545.0, 20.0, 0.9}},
    {{700.0, 15.0, -0.5}}, {{900.0, 12.0, 0.5}}, {{1150.0, 12.0, -0.4}}, {{1500.0, 10.0, 0.4}},
    {{2500.0, 2.5, 1.0}}, {{3600.0, 4.0, -0.3}},
}};
constexpr std::array<double, 4> familyScale{1.0, 0.82, 0.42, 0.26};
// Seats, -1 left to 1 right: violins left, violas centre, cellos right, basses further right.
constexpr std::array<double, 4> familySeat{-0.6, 0.15, 0.45, 0.75};

// splitmix64 to [0, 1): seeded by key and player, so a note's section is the same every render.
double unit(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return static_cast<double>((x ^ (x >> 31)) >> 11) * 0x1.0p-53;
}

} // namespace

StringsEngine::StringsEngine() {
    parameters_[index(StringsParameterId::outputGain)] = 0.7;
    parameters_[index(StringsParameterId::section)] = 6.0;
    parameters_[index(StringsParameterId::mute)] = 1.0; // the score's strings are con sordino
    parameters_[index(StringsParameterId::sulTasto)] = 0.0;
    parameters_[index(StringsParameterId::tremolo)] = 0.0;
    parameters_[index(StringsParameterId::harmonics)] = 0.0;
    parameters_[index(StringsParameterId::vibratoDepth)] = 1.0;
    parameters_[index(StringsParameterId::vibratoRate)] = 5.5;
    parameters_[index(StringsParameterId::brightness)] = 1.0;
    parameters_[index(StringsParameterId::spread)] = 1.0;
    parameters_[index(StringsParameterId::stereoWidth)] = 1.0;
    parameters_[index(StringsParameterId::masterTune)] = 440.0;
}

void StringsEngine::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    // Room for the lowest E at A = 415 Hz, bent down a tone, with vibrato.
    lineSize_ = 1;
    while (lineSize_ < static_cast<std::size_t>(sampleRate / 30.0) + 16) {
        lineSize_ *= 2;
    }
    pool_.assign(voiceCapacity * playerLimit * 2 * lineSize_, 0.0F);
    for (std::size_t f = 0; f < familyCount; ++f) {
        for (std::size_t i = 0; i < bodyModeCount; ++i) {
            const bool hill = i == 8;
            const double scale = hill ? std::sqrt(familyScale[f]) : familyScale[f];
            body_[f].coefficients[i] = bandpass(violinBody[i][0] * scale, violinBody[i][1], sampleRate);
            body_[f].gain[i] = violinBody[i][2];
        }
    }
    reset();
}

void StringsEngine::reset() {
    for (auto& voice : voices_) {
        voice = Voice{};
    }
    for (auto& body : body_) {
        body.state = {};
        body.mute = {};
    }
    bend_ = {};
    sustain_ = {};
    bowTarget_ = 0.6;
    vibratoAmount_ = 1.0;
    expression_ = expressionTarget_ = 1.0;
    endedCount_ = 0;
}

float* StringsEngine::line(std::size_t voice, std::size_t player, std::size_t which) {
    return pool_.data() + ((voice * playerLimit + player) * 2 + which) * lineSize_;
}

void StringsEngine::endNote(const NoteAddress& address) {
    if (endedCount_ < ended_.size()) {
        ended_[endedCount_++] = {address, frame_};
    }
}

// Seats the note's players: each a little detuned, landing the bow a little late, bowing a little
// faster or slower, with a vibrato and tremolo of its own.
void StringsEngine::start(Voice& voice) {
    const auto& p = parameters_;
    const int key = voice.address.key;
    const double spread = std::clamp(p[index(StringsParameterId::spread)], 0.0, 2.0);
    const double rate = std::clamp(p[index(StringsParameterId::vibratoRate)], 3.0, 9.0);
    voice.family = familyFor(key);
    voice.players = static_cast<std::size_t>(std::clamp(std::lround(p[index(StringsParameterId::section)]), 1L,
                                                        static_cast<long>(playerLimit)));
    const double frequency = p[index(StringsParameterId::masterTune)] * std::pow(2.0, (key - 69) / 12.0);
    // The bowed loop runs about 2 cents sharp per kHz at mid bow; taken back.
    voice.period = sampleRate_ / frequency * std::pow(2.0, 2.0 * frequency / 1000.0 / 1200.0);
    std::size_t size = 1;
    while (size < static_cast<std::size_t>(voice.period * 1.25) + 16) {
        size *= 2;
    }
    voice.mask = std::min(size, lineSize_) - 1;
    const auto v = static_cast<std::size_t>(&voice - voices_.data());
    for (std::size_t i = 0; i < voice.players; ++i) {
        auto& player = voice.player[i];
        const auto seed = static_cast<std::uint64_t>(key) * 1000u + i * 10u;
        const double solo = voice.players == 1 ? 0.0 : 1.0;
        player = Player{};
        player.detune = std::pow(2.0, -solo * spread * 12.0 * (unit(seed) - 0.5) / 1200.0);
        player.onset = i == 0 ? 0.0 : spread * 0.03 * unit(seed + 1);
        player.bowScale = 1.0 + solo * spread * 0.15 * (unit(seed + 2) - 0.5);
        player.bowPoint = 1.0 + solo * spread * 0.3 * (unit(seed + 7) - 0.5);
        player.driftSeed = seed + 8;
        player.armSeed = seed + 9;
        player.rosin = static_cast<std::uint32_t>(seed * 2654435761u) | 1u;
        player.vibratoStep = 2.0 * filters::pi * rate * (1.0 + 0.12 * (unit(seed + 3) - 0.5)) / sampleRate_;
        player.vibratoPhase = 2.0 * filters::pi * unit(seed + 4);
        player.tremoloStep = 2.0 * filters::pi * 7.0 * (1.0 + 0.1 * (unit(seed + 5) - 0.5)) / sampleRate_;
        player.tremoloPhase = 2.0 * filters::pi * unit(seed + 6);
        player.seat = voice.players == 1 ? 0.0 : 2.0 * static_cast<double>(i) / static_cast<double>(voice.players - 1) - 1.0;
        std::fill_n(line(v, i, 0), voice.mask + 1, 0.0F);
        std::fill_n(line(v, i, 1), voice.mask + 1, 0.0F);
    }
}

void StringsEngine::release(Voice& voice) {
    voice.keyDown = false;
    const auto channel = static_cast<std::size_t>(std::clamp<int>(voice.address.channel, 0, 15));
    voice.held = sustain_[channel];
    if (!voice.held) {
        voice.releasedAt = voice.time;
    }
}

void StringsEngine::noteOn(const NoteAddress& address, double velocity) {
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
        // Every desk busy: give up the quietest released note, else the quietest.
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
    voice->bow = bowTarget_;
    start(*voice);
}

void StringsEngine::noteOff(const NoteAddress& address, double) {
    for (auto& voice : voices_) {
        if (voice.active && voice.keyDown && matches(address, voice.address)) {
            release(voice);
        }
    }
}

void StringsEngine::choke(const NoteAddress& address) {
    for (auto& voice : voices_) {
        if (voice.active && matches(address, voice.address)) {
            endNote(voice.address);
            voice = Voice{};
        }
    }
}

void StringsEngine::controlChange(std::int16_t channel, std::uint8_t controller, double value) {
    value = std::clamp(value, 0.0, 1.0);
    if (controller == 1) {
        bowTarget_ = value;
    } else if (controller == 11) {
        expressionTarget_ = value;
    } else if (controller == 21) {
        vibratoAmount_ = 2.0 * value;
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

void StringsEngine::pitchBend(std::int16_t channel, double bend) {
    if (channel >= 0 && channel < 16 && std::isfinite(bend)) {
        bend_[static_cast<std::size_t>(channel)] = std::clamp(bend, -1.0, 1.0);
    }
}

void StringsEngine::setParameter(StringsParameterId id, double value) {
    const auto i = index(id);
    if (i != 0 && i < parameterCount && std::isfinite(value)) {
        parameters_[i] = value;
    }
}

double StringsEngine::parameterValue(StringsParameterId id) const {
    const auto i = index(id);
    return i < parameterCount ? parameters_[i] : 0.0;
}

void StringsEngine::modulate(const NoteAddress& target, StringsParameterId id, double amount) {
    if (id != StringsParameterId::brightness || !std::isfinite(amount)) {
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

void StringsEngine::setNoteVolume(const NoteAddress& target, double gain) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(gain)) {
            voice.volumeTarget = std::clamp(gain, 0.0, 4.0);
        }
    }
}

void StringsEngine::setNotePan(const NoteAddress& target, double pan) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(pan)) {
            voice.pan = std::clamp(pan, 0.0, 1.0);
        }
    }
}

void StringsEngine::setNoteBrightness(const NoteAddress& target, double brightness) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(brightness)) {
            voice.timbre = 2.0 * (std::clamp(brightness, 0.0, 1.0) - 0.5);
        }
    }
}

void StringsEngine::setNotePressure(const NoteAddress& target, double pressure) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(pressure)) {
            voice.pressure = std::clamp(pressure, 0.0, 1.0);
        }
    }
}

std::size_t StringsEngine::takeEndedNotes(EndedNote* destination, std::size_t capacity) {
    const std::size_t count = std::min(capacity, endedCount_);
    std::copy_n(ended_.begin(), count, destination);
    endedCount_ = 0;
    return count;
}

std::size_t StringsEngine::activeVoiceCount() const {
    return static_cast<std::size_t>(std::count_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.active; }));
}

void StringsEngine::render(float* left, float* right, std::uint32_t frameCount) {
    const auto& p = parameters_;
    const double fs = sampleRate_;
    const double width = std::clamp(p[index(StringsParameterId::stereoWidth)], 0.0, 1.0);
    const bool muted = p[index(StringsParameterId::mute)] >= 0.5;
    const bool tremolo = p[index(StringsParameterId::tremolo)] >= 0.5;
    const bool harmonics = p[index(StringsParameterId::harmonics)] >= 0.5;
    const double tasto = std::clamp(p[index(StringsParameterId::sulTasto)], 0.0, 1.0);
    const double vibratoCents = 15.0 * std::max(p[index(StringsParameterId::vibratoDepth)], 0.0) * vibratoAmount_;
    const double output = p[index(StringsParameterId::outputGain)];
    const double nutGain = harmonics ? 0.97 : 0.995;
    const double smooth = 1.0 - std::exp(-1.0 / (0.02 * fs));
    const double glide = 1.0 - std::exp(-1.0 / (0.01 * fs));
    const double releaseRate = 1.0 - std::exp(-1.0 / (0.08 * fs));
    const double driftRate = 1.0 - std::exp(-1.0 / (0.15 * fs));
    const double armRate = 1.0 - std::exp(-1.0 / (0.25 * fs));
    const double rosinRate = 1.0 - std::exp(-2.0 * filters::pi * 2000.0 / fs);
    const double cornerRate = 1.0 - std::exp(-2.0 * filters::pi * 1500.0 / fs);
    const double solo = parameters_[index(StringsParameterId::section)] < 1.5 ? 0.0 : 1.0;
    const double spread = std::clamp(parameters_[index(StringsParameterId::spread)], 0.0, 2.0);

    // A mute on the bridge (con sordino) adds mass there: the bridge hill and everything above
    // about 1 kHz drop (Cremer, The Physics of the Violin). Its mass is a low-pass on what the
    // bridge passes to the body, as well as weaker high body modes.
    const double muteLowpass = muted ? 1.0 - std::exp(-2.0 * filters::pi * 1200.0 / fs) : 1.0;
    std::array<std::array<double, bodyModeCount>, familyCount> bodyGain{};
    for (std::size_t f = 0; f < familyCount; ++f) {
        for (std::size_t i = 0; i < bodyModeCount; ++i) {
            const double frequency = violinBody[i][0] * (i == 8 ? std::sqrt(familyScale[f]) : familyScale[f]);
            bodyGain[f][i] = body_[f].gain[i] * (muted && frequency > 1000.0 ? 0.25 : 1.0) * (muted ? 0.7 : 1.0);
        }
    }

    struct Block {
        double attack, loss, lossDelay, pressureScale, gain, bend, beta, brightness; // bend divides the period, with tuning
    };
    std::array<Block, voiceCapacity> block{};
    for (std::size_t v = 0; v < voiceCapacity; ++v) {
        auto& voice = voices_[v];
        if (!voice.active) {
            continue;
        }
        const double brightness = std::clamp(p[index(StringsParameterId::brightness)] +
                                                 globalModulation_[index(StringsParameterId::brightness)] +
                                                 voice.brightnessModulation + voice.timbre, 0.25, 2.0);
        // The bridge's loss: a one-pole low-pass, darker at low Brightness. A harder, faster bow
        // sharpens the Helmholtz corner that the losses round off (Cremer), so CC1 lightens it.
        const double pole = std::pow((harmonics ? 0.65 : std::clamp(0.6 - 0.4 * voice.bow - 0.2 * (brightness - 1.0), 0.05, 0.85)), 48000.0 / fs);
        const double omega = 2.0 * filters::pi / voice.period;
        block[v] = {1.0 - std::exp(-1.0 / ((0.25 - 0.21 * voice.velocity) * fs)), pole,
                    phaseDelay(1.0 - pole, 0.0, pole, omega),
                    (0.6 + 0.4 * brightness) * (1.0 - 0.4 * tasto) * (harmonics ? 0.5 : 1.0),
                    std::pow(static_cast<double>(voice.players), -0.3), // a section is a little louder than one player
                    std::pow(2.0, 2.0 * bend_[static_cast<std::size_t>(std::clamp<int>(voice.address.channel, 0, 15))] / 12.0 -
                             (harmonics ? 8.5 * fs / voice.period / 1000.0 / 1200.0 : 0.0))};
        // (Harmonics' heavy damping rounds the Helmholtz corner, so the slip comes early: 8.5
        // cents sharp per kHz, taken back.)
        // The bow's point, as a share of the string from the bridge: players bow nearer the bridge to
        // play louder (Schelleng; Askenfelt), from 0.13 at pp to 0.10 at ff, and over the fingerboard
        // sul tasto. Harmonics are bowed lightly there, and the touching finger damps the upper modes.
        block[v].brightness = brightness;
        block[v].beta = harmonics ? 0.3 : 0.13 - 0.03 * voice.bow + 0.15 * tasto;
        for (std::size_t i = 0; i < voice.players; ++i) {
            auto& player = voice.player[i];
            const double place = 0.5 + 0.5 * width * std::clamp(familySeat[voice.family] + 0.15 * player.seat, -1.0, 1.0) +
                                 (voice.pan - 0.5);
            // cos on both sides, so the centre is equal to the bit (Stereo Width 0 is exact mono).
            player.gainLeft = std::cos(std::clamp(place, 0.0, 1.0) * 0.5 * filters::pi) * block[v].gain;
            player.gainRight = std::cos(std::clamp(1.0 - place, 0.0, 1.0) * 0.5 * filters::pi) * block[v].gain;
        }
    }

    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        frame_ = frame;
        expression_ += glide * (expressionTarget_ - expression_);
        std::array<double, familyCount> sumLeft{}, sumRight{};
        for (std::size_t v = 0; v < voiceCapacity; ++v) {
            auto& voice = voices_[v];
            if (!voice.active) {
                continue;
            }
            const auto& b = block[v];
            const bool gate = voice.keyDown || voice.held;
            // CC1 is the bow, or the note's own Pressure when it has one: faster and pressed harder
            // together, as players do (Schelleng).
            voice.bow += smooth * ((voice.pressure >= 0.0 ? voice.pressure : bowTarget_) - voice.bow);
            const double speed = 0.03 + 0.17 * voice.bow;
            const double pressure = 0.25 + 0.75 * voice.bow;
            const double slope = 5.0 - 4.0 * std::clamp(pressure * b.pressureScale, 0.0, 1.0);
            // A harder bow sharpens the Helmholtz corner that the losses round off (Cremer), so more
            // of the upper partials survive: a shelf above 1.5 kHz, -2.4 dB at CC1 0.3 to +4 dB at 1.
            // Brightness (and Timbre) moves the same shelf, -12 dB at 0.25 to +6 dB at 2: the bridge
            // loss and bow force it also sets change a steady bowed tone by under a dB.
            const double edge = std::max(1.2 * (voice.bow - 0.5) + (b.brightness - 1.0), -0.9);
            voice.volume += glide * (voice.volumeTarget - voice.volume);
            double sample = 0.0;
            for (std::size_t i = 0; i < voice.players; ++i) {
                auto& player = voice.player[i];
                const double t = voice.time - player.onset;
                if (t < 0.0) {
                    continue;
                }
                player.envelope += (gate ? b.attack : releaseRate) * ((gate ? 1.0 : 0.0) - player.envelope);
                // A harder attack bites: the bow starts faster and settles within ~80 ms.
                // No arm is steady: the bow's speed, and the vibrato hand with it, wander a little,
                // a new way every quarter second or so.
                if ((player.armClock -= 1.0 / fs) <= 0.0) {
                    player.armTarget = 2.0 * unit(player.armSeed++) - 1.0;
                    player.armClock = 0.15 + 0.3 * unit(player.armSeed++);
                }
                player.arm += armRate * (player.armTarget - player.arm);
                // Rosin: the friction is noisy, so no two slips are alike (McIntyre, Schumacher and
                // Woodhouse, "Aperiodicity in bowed-string motion", 1981).
                player.rosin ^= player.rosin << 13;
                player.rosin ^= player.rosin >> 17;
                player.rosin ^= player.rosin << 5;
                player.rosinLow += rosinRate * (static_cast<double>(player.rosin) / 2147483648.0 - 1.0 - player.rosinLow);
                double bowSpeed = player.envelope * speed * player.bowScale * (1.0 + 0.05 * player.arm) *
                                  (1.0 + 0.6 * voice.velocity * std::exp(-t / 0.08));
                double grip = slope;
                if (tremolo) {
                    // Bow reversals, about 14 a second, each player at their own pace: sharp turns,
                    // the hand light as the bow turns, so the string rings through each one instead
                    // of being gripped by a slow, heavy bow and rebuilt from nothing every stroke.
                    // The strokes are short and fast, 1.4x a sustained bow.
                    const double stroke = std::tanh(6.0 * std::sin(player.tremoloPhase));
                    bowSpeed *= 1.4 * stroke;
                    grip = 5.0 - (5.0 - slope) * (0.3 + 0.7 * std::abs(stroke));
                    player.tremoloPhase = std::fmod(player.tremoloPhase + player.tremoloStep, 2.0 * filters::pi);
                }
                // Vibrato in octaves; a few cents, so 2^-x to second order.
                const double x = vibratoCents * (1.0 + 0.3 * player.arm) * std::clamp((t - 0.2) / 0.5, 0.0, 1.0) *
                                 std::sin(player.vibratoPhase) / 1200.0 * 0.6931471805599453;
                player.vibratoPhase = std::fmod(player.vibratoPhase + player.vibratoStep * (1.0 + 0.08 * player.arm), 2.0 * filters::pi);
                // Each player's intonation wanders a few cents every ~quarter second, so no two
                // stay locked into a slow beat.
                if ((player.driftClock -= 1.0 / fs) <= 0.0) {
                    player.driftTarget = solo * spread * 4.0 * (unit(player.driftSeed++) - 0.5) / 1200.0;
                    player.driftClock = 0.15 + 0.2 * unit(player.driftSeed++);
                }
                player.drift += driftRate * (player.driftTarget - player.drift);
                const double y = x - player.drift * 0.6931471805599453;
                const double period = voice.period * player.detune / b.bend * (1.0 - y + 0.5 * y * y);

                float* neck = line(v, i, 0);
                float* bridge = line(v, i, 1);
                const double toBridge = std::max(std::min(b.beta * player.bowPoint, 0.45) * period, 2.0); // a cubic read needs two
                const double arriving = lagrange(bridge, voice.mask, player.write, toBridge);
                player.loss = (1.0 - b.loss) * arriving + b.loss * player.loss;
                const double fromBridge = -0.99 * player.loss;
                const double fromNut = -nutGain * lagrange(neck, voice.mask, player.write, period - toBridge - b.lossDelay);
                // The bow's friction (J. O. Smith's bow table): the string sticks to the bow until
                // their velocities differ enough, then slips.
                // The bow stays on the string as it stops, damping it, as players end a note.
                const double difference = bowSpeed - (fromBridge + fromNut);
                const double friction = std::abs(difference) * grip * (1.0 + 0.3 * player.rosinLow) + 0.75;
                const double added = difference * std::min(1.0, 1.0 / (friction * friction * friction * friction));
                neck[player.write] = static_cast<float>(fromBridge + added);
                bridge[player.write] = static_cast<float>(fromNut + added);
                player.write = (player.write + 1) & voice.mask;
                player.corner += cornerRate * (arriving - player.corner);
                const double radiated = (arriving + edge * (arriving - player.corner)) * voice.volume;
                sumLeft[voice.family] += radiated * player.gainLeft;
                sumRight[voice.family] += radiated * player.gainRight;
                sample += radiated * b.gain;
            }
            voice.time += 1.0 / fs;
            voice.level = std::max(std::abs(sample), voice.level * 0.9995);
            if (!gate && voice.releasedAt >= 0.0 && voice.time - voice.releasedAt > 0.1 && voice.level < 1.0e-5) {
                endNote(voice.address);
                voice = Voice{};
            }
        }
        double outLeft = 0.0, outRight = 0.0;
        for (std::size_t f = 0; f < familyCount; ++f) {
            auto& body = body_[f];
            body.mute[0] += muteLowpass * (sumLeft[f] - body.mute[0]);
            body.mute[1] += muteLowpass * (sumRight[f] - body.mute[1]);
            double l = 0.2 * body.mute[0], r = 0.2 * body.mute[1];
            for (std::size_t i = 0; i < bodyModeCount; ++i) {
                l += bodyGain[f][i] * biquad(body.coefficients[i], body.mute[0], body.state[0][2 * i], body.state[0][2 * i + 1]);
                r += bodyGain[f][i] * biquad(body.coefficients[i], body.mute[1], body.state[1][2 * i], body.state[1][2 * i + 1]);
            }
            outLeft += l;
            outRight += r;
        }
        const double gain = 1.2 * output * expression_;
        left[frame] = static_cast<float>(gain * outLeft);
        right[frame] = static_cast<float>(gain * outRight);
    }
    frame_ = 0;
}

} // namespace pianocraft
