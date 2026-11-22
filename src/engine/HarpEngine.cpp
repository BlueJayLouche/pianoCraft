#include "engine/HarpEngine.hpp"

#include <algorithm>
#include <cmath>

namespace pianocraft {

namespace {

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double ln1000 = 6.907755278982138; // T60: a decay of 60 dB
constexpr int lowestKey = 23;                // B0, the C1 string with its C-flat pedal
constexpr int highestKey = 104;              // G7, and G#7 on its sharp pedal
constexpr std::size_t partialLimit = 40;
constexpr std::size_t polarizedPartials = 12;

std::size_t index(HarpParameterId id) {
    return static_cast<std::size_t>(id);
}

// Soundbox modes (frequency, Q, signed gain): analytic, set by ear for a concert harp's box and
// board, the lowest near the sound holes' air resonance.
constexpr std::array<std::array<double, 3>, 12> bodyModes{{
    {{125.0, 10.0, 1.0}}, {{185.0, 14.0, -0.7}}, {{240.0, 12.0, 0.6}}, {{330.0, 16.0, -0.5}},
    {{420.0, 18.0, 0.5}}, {{560.0, 20.0, -0.4}}, {{720.0, 22.0, 0.35}}, {{930.0, 24.0, -0.3}},
    {{1180.0, 26.0, 0.25}}, {{1500.0, 28.0, -0.2}}, {{1950.0, 30.0, 0.18}}, {{2600.0, 30.0, -0.15}},
}};

double biquad(const std::array<double, 5>& c, std::array<double, 4>& state, double input) {
    const double output = c[0] * input + c[1] * state[0] + c[2] * state[1] - c[3] * state[2] - c[4] * state[3];
    state[1] = state[0];
    state[0] = input;
    state[3] = state[2];
    state[2] = output;
    return output;
}

double radiusFor(double t60, double sampleRate) {
    return std::exp(-ln1000 / (t60 * sampleRate));
}

// Partial n of a string at A4 = 440 Hz: its frequency (gut and nylon nearly harmonic, short treble
// strings stiffer) and its T60 x Sustain Length. Fitted to VCSL's concert harp (tools/fit_harp.py): T60 falls
// as (f / 440)^-0.92, and a lighter string loses more to the soundboard at the same frequency, 2^(0.26 / octave)
// faster up the compass (its impedance falls). The wound bass strings are the slowest; the old by-ear wire loss
// made them die 3-8x too fast.
std::pair<double, double> partial(int stringKey, int n, double sustain) {
    const double fundamental = 440.0 * std::pow(2.0, (stringKey - 69) / 12.0);
    const double inharmonicity = 1.5e-5 * std::pow(2.0, (stringKey - 60) / 24.0);
    const double k = static_cast<double>(n);
    const double frequency = k * fundamental * std::sqrt((1.0 + inharmonicity * k * k) / (1.0 + inharmonicity));
    const double t60 = std::clamp(sustain * 5.63 * std::pow(frequency / 440.0, -0.922) *
                                      std::exp2(-0.256 * (stringKey - 60) / 12.0), 0.08, 35.0);
    return {frequency, t60};
}

// How well the board radiates a partial: weak below ~120 Hz, gently falling above 6 kHz.
double radiation(double frequency) {
    return (frequency / 120.0) / std::sqrt(1.0 + std::pow(frequency / 120.0, 2.0)) / std::sqrt(1.0 + std::pow(frequency / 6000.0, 2.0));
}

// Sympathetic coupling: a string partial in tune with a partial in the board's motion settles at
// this share of it.
constexpr double sympathy = 0.2;

} // namespace

HarpEngine::HarpEngine() {
    parameters_[index(HarpParameterId::outputGain)] = 0.7;
    parameters_[index(HarpParameterId::pluckPosition)] = 0.35;
    parameters_[index(HarpParameterId::presDeLaTable)] = 0.0;
    parameters_[index(HarpParameterId::harmonics)] = 0.0;
    parameters_[index(HarpParameterId::brightness)] = 1.0;
    parameters_[index(HarpParameterId::damping)] = 0.0;
    parameters_[index(HarpParameterId::sustainLength)] = 1.0;
    parameters_[index(HarpParameterId::bodyLevel)] = 1.0;
    parameters_[index(HarpParameterId::stereoWidth)] = 1.0;
    parameters_[index(HarpParameterId::masterTune)] = 440.0;
    parameters_[index(HarpParameterId::sympatheticLevel)] = 1.0;
}

void HarpEngine::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    for (std::size_t i = 0; i < bodyModeCount; ++i) {
        // RBJ band-pass with unit peak gain.
        const double omega = 2.0 * pi * std::min(bodyModes[i][0], 0.45 * sampleRate) / sampleRate;
        const double alpha = std::sin(omega) / (2.0 * bodyModes[i][1]);
        const double a0 = 1.0 + alpha;
        body_[i].coefficients = {alpha / a0, 0.0, -alpha / a0, -2.0 * std::cos(omega) / a0, (1.0 - alpha) / a0};
        body_[i].gain = bodyModes[i][2];
    }
    reset();
    tuneSympathy();
}

// Every string rings in sympathy, its first partials tuned and damped as a plucked one.
void HarpEngine::tuneSympathy() {
    const double sustain = parameters_[index(HarpParameterId::sustainLength)];
    const double tune = parameters_[index(HarpParameterId::masterTune)] / 440.0;
    for (std::size_t s = 0; s < stringCount; ++s) {
        auto& string = sympathy_[s];
        string.count = 0;
        for (int n = 1; n <= static_cast<int>(sympatheticPartials); ++n) {
            const auto [frequency, t60] = partial(lowestString + static_cast<int>(s), n, sustain);
            const double omega = 2.0 * pi * frequency * tune / sampleRate_;
            if (omega > 0.9 * pi) {
                break;
            }
            auto& mode = string.modes[string.count];
            mode.frequency = frequency;
            mode.radius = radiusFor(t60, sampleRate_);
            mode.a1 = 2.0 * mode.radius * std::cos(omega);
            mode.a2 = mode.radius * mode.radius;
            // Normalised so the resonance's gain at its own frequency is `sympathy`.
            string.drive[string.count] = sympathy * (1.0 - mode.radius) * 2.0 * std::sin(omega);
            string.radiation[string.count++] = radiation(frequency);
        }
    }
}

// A harp has seven strings an octave, one per letter, set flat, natural or sharp by its pedals. A
// note outside the setting moves a pedal: it takes the place of the neighbour (sharing its letter)
// played least recently.
void HarpEngine::setPedal(int pitchClass) {
    const auto pc = static_cast<std::size_t>(pitchClass % 12);
    pedalUsed_[pc] = ++clock_;
    if (pedals_[pc]) {
        return;
    }
    const std::size_t below = (pc + 11) % 12, above = (pc + 1) % 12;
    std::size_t out = pedals_[below] && (!pedals_[above] || pedalUsed_[below] <= pedalUsed_[above]) ? below : above;
    if (!pedals_[out]) {
        // ponytail: neither neighbour sounds, so give up the least recently played pitch class.
        for (std::size_t i = 0; i < 12; ++i) {
            out = pedals_[i] && (!pedals_[out] || pedalUsed_[i] < pedalUsed_[out]) ? i : out;
        }
    }
    pedals_[out] = false;
    pedals_[pc] = true;
}

void HarpEngine::reset() {
    for (auto& voice : voices_) {
        voice = Voice{};
    }
    for (auto& mode : body_) {
        mode.state = {};
    }
    bend_ = {};
    sustain_ = {};
    expression_ = expressionTarget_ = 1.0;
    endedCount_ = 0;
    for (auto& string : sympathy_) {
        for (auto& mode : string.modes) {
            mode.y1 = mode.y2 = 0.0;
        }
        string.level = 0.0;
    }
    pedals_ = {true, false, true, false, true, true, false, true, false, true, false, true}; // C major
    pedalUsed_ = {};
    clock_ = 0;
}

double HarpEngine::tuningFactor(std::int16_t channel) const {
    const double bend = channel >= 0 && channel < 16 ? bend_[static_cast<std::size_t>(channel)] : 0.0;
    return parameters_[index(HarpParameterId::masterTune)] / 440.0 * std::pow(2.0, 2.0 * bend / 12.0);
}

void HarpEngine::retune(Voice& voice) {
    const double glide = voice.glideCents * std::exp(-static_cast<double>(voice.age) / voice.glideSamples);
    const double factor = tuningFactor(voice.address.channel) * std::pow(2.0, glide / 1200.0);
    for (std::size_t i = 0; i < voice.modeCount; ++i) {
        auto& mode = voice.modes[i];
        const double omega = 2.0 * pi * mode.frequency * factor / sampleRate_;
        mode.a1 = omega < 0.95 * pi ? 2.0 * mode.radius * std::cos(omega) : 0.0;
        mode.a2 = omega < 0.95 * pi ? mode.radius * mode.radius : 0.0;
    }
}

void HarpEngine::endNote(const NoteAddress& address) {
    if (endedCount_ < ended_.size()) {
        ended_[endedCount_++] = {address, frame_};
    }
}

// The string starts from the finger's triangle at the pluck point, released at rest: partial n
// pushes the board with sin(n pi beta) / n, falling 12 dB an octave above a corner set by the
// fingertip and how hard it pulls. Touching the middle of the string (harmonics) leaves the even partials of the
// string an octave down, so the key still names the sounding pitch.
void HarpEngine::pluck(Voice& voice, double velocity) {
    const auto& p = parameters_;
    const auto& g = globalModulation_;
    const int key = voice.address.key;
    const double table = std::clamp(p[index(HarpParameterId::presDeLaTable)], 0.0, 1.0);
    const double position = std::clamp(p[index(HarpParameterId::pluckPosition)] + g[index(HarpParameterId::pluckPosition)] +
                                           voice.positionModulation, 0.03, 0.5);
    const double beta = position + (0.06 - position) * table;
    const double hardness = std::clamp(p[index(HarpParameterId::brightness)] + g[index(HarpParameterId::brightness)] +
                                           voice.brightnessModulation + voice.timbre, 0.25, 3.0);
    const bool harmonics = p[index(HarpParameterId::harmonics)] >= 0.5 && key - 12 >= lowestKey;
    const int stringKey = harmonics ? key - 12 : key;
    const double fundamental = 440.0 * std::pow(2.0, (stringKey - 69) / 12.0);
    const double corner = 3000.0 * hardness * (0.55 + 0.65 * velocity) * (1.0 + 1.5 * table) * (harmonics ? 0.5 : 1.0);
    const double sustain = p[index(HarpParameterId::sustainLength)];
    const double limit = std::min(0.45 * sampleRate_, 14000.0);

    std::array<Mode, modeLimit> modes{};
    std::array<double, modeLimit> amplitude{};
    std::size_t count = 0;
    double energy = 0.0;
    for (std::size_t n = 1; n <= partialLimit && count < modeLimit; ++n) {
        const double k = static_cast<double>(n);
        const auto [frequency, stringT60] = partial(stringKey, static_cast<int>(n), sustain);
        if (frequency > limit) {
            break;
        }
        double a = std::sin(k * pi * beta) / k / (1.0 + std::pow(frequency / corner, 2.0)) * radiation(frequency);
        const bool touched = harmonics && n % 2 == 1;
        // The touched string's odd partials only tick before the palm stops them.
        const double t60 = touched ? 0.05 : stringT60;
        if (n == 1) {
            // A hard pluck stretches a low string: up to 8 cents sharp, settling as its swing (squared) dies.
            voice.glideCents = 8.0 * velocity * velocity * std::clamp((76.0 - stringKey) / 36.0, 0.0, 1.0);
            voice.glideSamples = t60 / (2.0 * ln1000) * sampleRate_;
        }
        if (touched) {
            a *= 0.06;
        } else {
            energy += a * a;
        }
        modes[count] = {frequency, radiusFor(t60, sampleRate_)};
        amplitude[count++] = a;
        // The polarization along the board feels it less: weaker, slower, a hair sharp.
        if (n <= polarizedPartials && !touched && count < modeLimit) {
            modes[count] = {frequency * 1.0005, radiusFor(std::min(1.5 * t60, 40.0), sampleRate_)};
            amplitude[count++] = 0.2 * a;
        }
    }

    const double gain = 0.35 * (0.03 + 0.97 * std::pow(velocity, 1.7)) * (1.0 - 0.3 * table) /
                        std::sqrt(std::max(energy, 1.0e-12));
    // A ringing string re-plucked keeps a quarter of its motion: the finger catches it first.
    const bool sameString = voice.modeCount == count && count > 0 && voice.modes[0].frequency == modes[0].frequency;
    for (std::size_t i = 0; i < count; ++i) {
        if (sameString) {
            modes[i].y1 = 0.25 * voice.modes[i].y1;
            modes[i].y2 = 0.25 * voice.modes[i].y2;
        }
    }
    voice.modes = modes;
    voice.modeCount = count;
    voice.damped = false;
    voice.stringKey = stringKey;
    if (stringKey >= lowestString && stringKey < lowestString + static_cast<int>(stringCount)) {
        auto& string = sympathy_[static_cast<std::size_t>(stringKey - lowestString)]; // the finger takes it
        for (auto& mode : string.modes) {
            mode.y1 = mode.y2 = 0.0;
        }
        string.level = 0.0;
    }
    retune(voice);
    const double factor = tuningFactor(voice.address.channel) * std::pow(2.0, voice.glideCents / 1200.0);
    double load = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        auto& mode = voice.modes[i];
        const double omega = 2.0 * pi * mode.frequency * factor / sampleRate_;
        mode.y1 += amplitude[i] * gain;
        mode.y2 += amplitude[i] * gain * std::cos(omega) / mode.radius;
        load += amplitude[i] * gain * mode.radius * std::cos(omega); // this mode's first output sample
    }
    voice.release += load;
    voice.releaseDecay = std::exp(-fundamental * factor / sampleRate_);
    const auto channel = static_cast<std::size_t>(std::clamp<int>(voice.address.channel, 0, 15));
    if (!voice.keyDown && !sustain_[channel]) {
        damp(voice); // released before it was plucked
    }
}

void HarpEngine::damp(Voice& voice) {
    const double damping = parameters_[index(HarpParameterId::damping)];
    if (damping <= 0.0 || voice.damped) {
        return;
    }
    // The hand adds decay: at Damping 1 every partial dies in about 0.12 s.
    for (std::size_t i = 0; i < voice.modeCount; ++i) {
        auto& mode = voice.modes[i];
        const double rate = -std::log(mode.radius) * sampleRate_ / ln1000 + damping / 0.12;
        mode.radius = std::exp(-ln1000 * rate / sampleRate_);
    }
    voice.damped = true;
    retune(voice);
}

void HarpEngine::noteOn(const NoteAddress& address, double velocity) {
    if (address.key < lowestKey || address.key > highestKey) {
        endNote(address);
        return;
    }
    Voice* voice = nullptr;
    for (auto& candidate : voices_) {
        if (candidate.active && candidate.address.key == address.key) {
            voice = &candidate;
            endNote(candidate.address);
            break;
        }
    }
    if (voice == nullptr) {
        for (auto& candidate : voices_) {
            if (!candidate.active) {
                voice = &candidate;
                *voice = Voice{};
                break;
            }
        }
    }
    if (voice == nullptr) {
        // Every string ringing: give up the quietest released one, else the quietest.
        voice = &voices_[0];
        for (auto& candidate : voices_) {
            const bool better = (!candidate.keyDown && voice->keyDown) ||
                                (candidate.keyDown == voice->keyDown && candidate.level < voice->level);
            voice = better ? &candidate : voice;
        }
        endNote(voice->address);
        *voice = Voice{};
    }
    voice->address = address;
    voice->active = true;
    voice->keyDown = true;
    voice->age = 0;
    voice->volumeTarget = 1.0;
    voice->pan = 0.5;
    voice->brightnessModulation = voice->positionModulation = voice->timbre = 0.0;
    voice->pendingVelocity = std::clamp(velocity, 0.0, 1.0);
    setPedal(address.key);
}

void HarpEngine::noteOff(const NoteAddress& address, double) {
    for (auto& voice : voices_) {
        if (voice.active && voice.keyDown && matches(address, voice.address)) {
            voice.keyDown = false;
            const auto channel = static_cast<std::size_t>(std::clamp<int>(voice.address.channel, 0, 15));
            if (!sustain_[channel]) {
                damp(voice);
            }
        }
    }
}

void HarpEngine::choke(const NoteAddress& address) {
    for (auto& voice : voices_) {
        if (voice.active && matches(address, voice.address)) {
            endNote(voice.address);
            voice = Voice{};
        }
    }
}

void HarpEngine::controlChange(std::int16_t channel, std::uint8_t controller, double value) {
    if (controller == 11) {
        expressionTarget_ = std::clamp(value, 0.0, 1.0);
    } else if (controller == 64 && channel >= 0 && channel < 16) {
        sustain_[static_cast<std::size_t>(channel)] = value >= 0.5;
        if (value < 0.5) {
            for (auto& voice : voices_) {
                if (voice.active && !voice.keyDown && voice.address.channel == channel) {
                    damp(voice);
                }
            }
        }
    }
}

void HarpEngine::pitchBend(std::int16_t channel, double bend) {
    if (channel < 0 || channel >= 16 || !std::isfinite(bend)) {
        return;
    }
    bend_[static_cast<std::size_t>(channel)] = std::clamp(bend, -1.0, 1.0);
    for (auto& voice : voices_) {
        if (voice.active && voice.address.channel == channel && voice.pendingVelocity < 0.0) {
            retune(voice);
        }
    }
}

void HarpEngine::setParameter(HarpParameterId id, double value) {
    const auto i = index(id);
    if (i == 0 || i >= parameterCount || !std::isfinite(value)) {
        return;
    }
    parameters_[i] = value;
    if (id == HarpParameterId::masterTune || id == HarpParameterId::sustainLength) {
        tuneSympathy();
    }
    if (id == HarpParameterId::masterTune) {
        for (auto& voice : voices_) {
            if (voice.active && voice.pendingVelocity < 0.0) {
                retune(voice);
            }
        }
    }
}

double HarpEngine::parameterValue(HarpParameterId id) const {
    const auto i = index(id);
    return i < parameterCount ? parameters_[i] : 0.0;
}

void HarpEngine::modulate(const NoteAddress& target, HarpParameterId id, double amount) {
    if (!std::isfinite(amount) || (id != HarpParameterId::brightness && id != HarpParameterId::pluckPosition)) {
        return;
    }
    if (target.noteId < 0 && target.port < 0 && target.channel < 0 && target.key < 0) {
        globalModulation_[index(id)] = amount;
        return;
    }
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address)) {
            (id == HarpParameterId::brightness ? voice.brightnessModulation : voice.positionModulation) = amount;
        }
    }
}

void HarpEngine::setNoteVolume(const NoteAddress& target, double gain) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(gain)) {
            voice.volumeTarget = std::clamp(gain, 0.0, 4.0);
        }
    }
}

void HarpEngine::setNotePan(const NoteAddress& target, double pan) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(pan)) {
            voice.pan = std::clamp(pan, 0.0, 1.0);
        }
    }
}

void HarpEngine::setNoteBrightness(const NoteAddress& target, double brightness) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(brightness)) {
            voice.timbre = 2.0 * (std::clamp(brightness, 0.0, 1.0) - 0.5);
        }
    }
}

std::size_t HarpEngine::takeEndedNotes(EndedNote* destination, std::size_t capacity) {
    const std::size_t count = std::min(capacity, endedCount_);
    std::copy_n(ended_.begin(), count, destination);
    endedCount_ = 0;
    return count;
}

std::size_t HarpEngine::activeVoiceCount() const {
    return static_cast<std::size_t>(std::count_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.active; }));
}

void HarpEngine::render(float* left, float* right, std::uint32_t frameCount) {
    const double width = std::clamp(parameters_[index(HarpParameterId::stereoWidth)], 0.0, 1.0);
    const double bodyLevel = parameters_[index(HarpParameterId::bodyLevel)];
    const double output = parameters_[index(HarpParameterId::outputGain)];
    // Bass strings to the left, treble to the right, as the audience sees the harp; amplitude
    // panning only, so a mono sum can't comb-filter.
    std::array<double, voiceCapacity> gainLeft{};
    std::array<double, voiceCapacity> gainRight{};
    for (std::size_t v = 0; v < voiceCapacity; ++v) {
        auto& voice = voices_[v];
        if (!voice.active) {
            continue;
        }
        if (voice.pendingVelocity >= 0.0) {
            pluck(voice, voice.pendingVelocity);
            voice.pendingVelocity = -1.0;
        } else if (voice.glideCents * std::exp(-static_cast<double>(voice.age) / voice.glideSamples) > 0.02) {
            retune(voice); // the tension glide, block by block
        }
        const double spread = std::clamp((voice.address.key - 63) / 40.0, -1.0, 1.0) * 0.35 * width;
        const double place = std::clamp(0.5 + spread + (voice.pan - 0.5), 0.0, 1.0);
        // cos on both sides, so the centre is equal to the bit (Stereo Width 0 is exact mono).
        gainLeft[v] = std::cos(place * 0.5 * pi);
        gainRight[v] = std::cos((1.0 - place) * 0.5 * pi);
    }
    // The sympathetic strings: placed as the plucked ones; driven unless the pedals don't sound them
    // or that string is the one being played.
    const double sympatheticLevel = std::max(parameters_[index(HarpParameterId::sympatheticLevel)], 0.0);
    std::array<bool, stringCount> driven{};
    std::array<double, stringCount> stringLeft{};
    std::array<double, stringCount> stringRight{};
    for (std::size_t s = 0; s < stringCount; ++s) {
        const int key = lowestString + static_cast<int>(s);
        driven[s] = sympatheticLevel > 0.0 && pedals_[static_cast<std::size_t>(key % 12)];
        const double place = 0.5 + std::clamp((key - 63) / 40.0, -1.0, 1.0) * 0.35 * width;
        stringLeft[s] = std::cos(place * 0.5 * pi) * sympatheticLevel;
        stringRight[s] = std::cos((1.0 - place) * 0.5 * pi) * sympatheticLevel;
    }
    for (const auto& voice : voices_) {
        if (voice.active && voice.stringKey >= lowestString && voice.stringKey < lowestString + static_cast<int>(stringCount)) {
            driven[static_cast<std::size_t>(voice.stringKey - lowestString)] = false;
        }
    }
    const double glide = 100.0 / sampleRate_;
    const double release = 0.05 * sampleRate_;
    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        frame_ = frame;
        double sumLeft = 0.0;
        double sumRight = 0.0;
        double mono = 0.0;
        for (std::size_t v = 0; v < voiceCapacity; ++v) {
            auto& voice = voices_[v];
            if (!voice.active) {
                continue;
            }
            double sample = 0.0;
            for (std::size_t i = 0; i < voice.modeCount; ++i) {
                auto& mode = voice.modes[i];
                const double y = mode.a1 * mode.y1 - mode.a2 * mode.y2;
                mode.y2 = mode.y1;
                mode.y1 = std::abs(y) < 1.0e-30 ? 0.0 : y;
                sample += mode.y1;
            }
            sample -= voice.release;
            voice.release *= voice.releaseDecay;
            voice.volume += glide * (voice.volumeTarget - voice.volume);
            sample *= voice.volume;
            voice.level = std::max(std::abs(sample), voice.level * 0.9999);
            sumLeft += sample * gainLeft[v];
            sumRight += sample * gainRight[v];
            mono += sample;
            if (++voice.age > release && voice.level < 1.0e-6) {
                endNote(voice.address);
                voice = Voice{};
            }
        }
        // The board's motion drives every undamped string; each resonates where a partial lands on
        // its own (octaves, fifths, a third's 5th partial a little off), and radiates through the
        // board in turn.
        double ringing = 0.0;
        for (std::size_t s = 0; s < stringCount; ++s) {
            auto& string = sympathy_[s];
            if (!driven[s] && string.level < 1.0e-7) {
                continue;
            }
            const double input = driven[s] ? mono : 0.0;
            double y = 0.0;
            for (std::size_t n = 0; n < string.count; ++n) {
                auto& mode = string.modes[n];
                const double v = mode.a1 * mode.y1 - mode.a2 * mode.y2 + string.drive[n] * input;
                mode.y2 = mode.y1;
                mode.y1 = std::abs(v) < 1.0e-30 ? 0.0 : v;
                y += v * string.radiation[n];
            }
            string.level = std::max(std::abs(y), string.level * 0.9995);
            sumLeft += y * stringLeft[s];
            sumRight += y * stringRight[s];
            ringing += y;
        }
        mono += ringing * sympatheticLevel;
        double box = 0.0;
        for (auto& mode : body_) {
            box += mode.gain * biquad(mode.coefficients, mode.state, mono);
        }
        expression_ += glide * (expressionTarget_ - expression_);
        const double gain = output * expression_;
        left[frame] = static_cast<float>(gain * (sumLeft + bodyLevel * box * 0.707));
        right[frame] = static_cast<float>(gain * (sumRight + bodyLevel * box * 0.707));
    }
    frame_ = 0;
}

} // namespace pianocraft
