#include "engine/PianoEngine.hpp"

#include "engine/Soundboard.hpp"
#include "engine/SympatheticBank.hpp"
#include "model/InstrumentModel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

namespace pianocraft {
namespace {

// More slots than keys: a restrike reuses its key's voice, so even a fully pedalled keyboard
// never forces an audible voice to be cut.
constexpr std::size_t voiceCount = 96;
constexpr std::size_t parameterCount = 14;

std::size_t parameterIndex(ParameterId id) {
    return static_cast<std::size_t>(id) < parameterCount ? static_cast<std::size_t>(id) : 0;
}

// RBJ high shelf as b0, b1, b2, a1, a2.
std::array<double, 5> highShelf(double frequency, double gainDb, double sampleRate) {
    const double a = std::pow(10.0, gainDb / 40.0);
    const double omega = 2.0 * 3.141592653589793 * frequency / sampleRate;
    const double cosine = std::cos(omega);
    const double root = 2.0 * std::sqrt(a) * std::sin(omega) / std::sqrt(2.0);
    const double a0 = (a + 1.0) - (a - 1.0) * cosine + root;
    return {a * ((a + 1.0) + (a - 1.0) * cosine + root) / a0,
            -2.0 * a * ((a - 1.0) + (a + 1.0) * cosine) / a0,
            a * ((a + 1.0) + (a - 1.0) * cosine - root) / a0,
            2.0 * ((a - 1.0) - (a + 1.0) * cosine) / a0,
            ((a + 1.0) - (a - 1.0) * cosine - root) / a0};
}

double biquad(const std::array<double, 5>& c, double* state, double input) {
    const double output = c[0] * input + c[1] * state[0] + c[2] * state[1] - c[3] * state[2] - c[4] * state[3];
    state[1] = state[0];
    state[0] = input;
    state[3] = state[2];
    state[2] = std::abs(output) < 1.0e-30 ? 0.0 : output;
    return state[2];
}

struct ModeState {
    double y1 = 0.0;
    double y2 = 0.0;
};

struct HammerState {
    double position = -0.004;
    double velocity = 0.0;
    double mass = 0.018;
    bool active = false;
};

struct Voice {
    const KeyModel* key = nullptr;
    NoteAddress address{};
    std::array<ModeState, modeLimit> modes{};
    HammerState hammer{};
    std::uint64_t age = 0;
    double displacement = 0.0;
    double level = 0.0;
    double velocityGain = 1.0;
    double stiffness = 60000.0;
    double releaseBlend = 0.0;
    double chokeGain = 1.0;
    bool active = false;
    bool keyDown = false;
    bool released = false;
    bool heldByPedal = false;
    bool choking = false;
    std::array<double, 8> highpassState{};
    std::array<double, 5> velocityShelf{1.0, 0.0, 0.0, 0.0, 0.0};
    std::array<double, 8> shelfState{};

    void clear() {
        key = nullptr;
        address = {};
        modes = {};
        hammer = {};
        age = 0;
        displacement = 0.0;
        level = 0.0;
        velocityGain = 1.0;
        stiffness = 60000.0;
        releaseBlend = 0.0;
        chokeGain = 1.0;
        active = false;
        keyDown = false;
        released = false;
        heldByPedal = false;
        choking = false;
        highpassState = {};
        velocityShelf = {1.0, 0.0, 0.0, 0.0, 0.0};
        shelfState = {};
    }
};

} // namespace

struct PianoEngine::Impl {
    std::unique_ptr<InstrumentModel> model;
    std::array<Voice, voiceCount> voices{};
    std::array<bool, 16> sustain{};
    std::array<bool, instrumentKeyCount> undamped{};
    std::array<double, parameterCount> parameters{};
    Soundboard soundboard;
    SympatheticBank sympathetic;
    double sampleRate = 48000.0;
    double smoothedGain = 0.7;
    double smoothedWidth = 1.0;
    MicPosition micPosition = MicPosition::spacedPair;
    bool voicingDirty = true;
    // ponytail: three one-pole high-passes at 100 Hz stand in for the soundboard's weak
    // radiation of low fundamentals; replace with a radiation model if the bass needs more.
    static constexpr std::size_t highpassStages = 3;
    std::array<double, 2 * highpassStages> highpassInput{};
    std::array<double, 2 * highpassStages> highpassOutput{};
    double highpassCoefficient = 1.0;

    Impl() {
        parameters.fill(0.0);
        parameters[parameterIndex(ParameterId::outputGain)] = 0.7;
        parameters[parameterIndex(ParameterId::hammerHardness)] = 1.0;
        parameters[parameterIndex(ParameterId::sympatheticLevel)] = 0.35;
        parameters[parameterIndex(ParameterId::bodyLevel)] = 1.0;
        parameters[parameterIndex(ParameterId::micPosition)] = 0.0;
        parameters[parameterIndex(ParameterId::stereoWidth)] = 1.0;
        for (auto id : {ParameterId::dynamicRange, ParameterId::velocityBrightness, ParameterId::sustainLength,
                        ParameterId::damperRelease, ParameterId::unisonDetune, ParameterId::aftersound,
                        ParameterId::hammerNoise}) {
            parameters[parameterIndex(id)] = 1.0;
        }
    }

    // ponytail: recomputed on the audio thread at the next render() after a change, at most once
    // per block: ~65 us, worst ~200 us at 48 kHz. Move off-thread if fast voicing automation glitches.
    void applyVoicing() {
        model->setVoicing({parameters[parameterIndex(ParameterId::sustainLength)],
                           parameters[parameterIndex(ParameterId::damperRelease)],
                           parameters[parameterIndex(ParameterId::unisonDetune)],
                           parameters[parameterIndex(ParameterId::aftersound)]});
        voicingDirty = false;
    }

    void applyMicPosition() {
        micPosition = static_cast<MicPosition>(
            static_cast<int>(parameters[parameterIndex(ParameterId::micPosition)]));
        // ponytail: recomputed in place on the audio thread, ~60 us (worst ~100 us) at 48 kHz, a few
        // percent of a 64-sample block, only when the enum changes. Precompute the three layouts if
        // fast mic automation ever glitches.
        model->setMicPosition(micPosition);
        soundboard.setMicPosition(micPosition);
    }

    void resetVoices() {
        for (auto& voice : voices) {
            voice.clear();
        }
        sustain.fill(false);
        undamped.fill(false);
        if (model != nullptr) {
            updateUndamped();
        }
        soundboard.reset();
        sympathetic.reset();
        highpassInput.fill(0.0);
        highpassOutput.fill(0.0);
        smoothedGain = parameters[parameterIndex(ParameterId::outputGain)];
        smoothedWidth = parameters[parameterIndex(ParameterId::stereoWidth)];
    }

    double highpass(double sample, std::size_t channel) {
        for (std::size_t stage = channel * highpassStages; stage < (channel + 1) * highpassStages; ++stage) {
            const double output = highpassCoefficient * (highpassOutput[stage] + sample - highpassInput[stage]);
            highpassInput[stage] = sample;
            highpassOutput[stage] = std::abs(output) < 1.0e-30 ? 0.0 : output;
            sample = highpassOutput[stage];
        }
        return sample;
    }

    // Strings free to ring sympathetically: all of them with the pedal down, otherwise the
    // damperless top keys. A string already sounding as a voice is excluded so it is not doubled.
    void updateUndamped() {
        const bool pedal = std::any_of(sustain.begin(), sustain.end(), [](bool engaged) { return engaged; });
        for (std::size_t index = 0; index < undamped.size(); ++index) {
            undamped[index] = pedal || !model->key(static_cast<int>(index) + 21).hasDamper;
        }
        for (const auto& voice : voices) {
            if (voice.active && voice.key != nullptr) {
                undamped[voice.address.key - 21] = false;
            }
        }
    }

    static bool matches(const Voice& voice, const NoteAddress& address) {
        if (!voice.active || voice.key == nullptr || voice.address.key != address.key ||
            voice.address.channel != address.channel || voice.address.port != address.port) {
            return false;
        }
        return address.noteId < 0 || voice.address.noteId == address.noteId;
    }

    Voice* findRestrike(const NoteAddress& address) {
        Voice* candidate = nullptr;
        for (auto& voice : voices) {
            if (matches(voice, address) && !voice.keyDown && !voice.choking) {
                if (candidate == nullptr || voice.age < candidate->age) {
                    candidate = &voice;
                }
            }
        }
        return candidate;
    }

    Voice* allocateVoice() {
        for (auto& voice : voices) {
            if (!voice.active) {
                voice.clear();
                return &voice;
            }
        }

        Voice* selected = &voices.front();
        double bestScore = std::numeric_limits<double>::infinity();
        for (auto& voice : voices) {
            double score = voice.level;
            if (!voice.released) {
                score *= 4.0;
            }
            if (voice.keyDown) {
                score *= 3.0;
            }
            if (voice.heldByPedal) {
                score *= 2.0;
            }
            if (score < bestScore || (score == bestScore && voice.age > selected->age)) {
                selected = &voice;
                bestScore = score;
            }
        }
        selected->clear();
        return selected;
    }

    void strike(Voice& voice, const NoteAddress& address, double velocity) {
        constexpr double referenceVelocity = 80.0 / 127.0;
        voice.key = &model->key(address.key);
        voice.address = address;
        voice.active = true;
        voice.keyDown = true;
        voice.released = false;
        voice.releaseBlend = 0.0;
        voice.heldByPedal = false;
        voice.choking = false;
        voice.chokeGain = 1.0;
        voice.age = 0;
        const double normalizedVelocity = velocity / referenceVelocity;
        const double referenceGain = 0.18 + 0.82 * std::pow(referenceVelocity, 1.35);
        const auto hammerSpeed = [](double v) { return 0.35 + 2.4 * std::pow(v, 0.85); };
        // Dynamic Range scales each note's dB distance from mf: the gain law's share directly, and
        // the hammer speed's own share through a compensating gain.
        const double dynamics = parameters[parameterIndex(ParameterId::dynamicRange)];
        voice.velocityGain = referenceGain *
            std::pow(normalizedVelocity, voice.key->velocityGainExponent * dynamics) *
            std::pow(hammerSpeed(velocity) / hammerSpeed(referenceVelocity), dynamics - 1.0) *
            voice.key->outputGain;
        // Effective excitation parameters, grid-fitted on octave-band spectra; recordings can't
        // separate mass from stiffness. Lighter, stiffer toward the treble keeps contact near 1-5 ms;
        // the old 18 g / 60 kN hammer stayed on for 8-15 ms and never excited the upper partials.
        const double registerPosition = (address.key - 21) / 87.0;
        voice.stiffness = 8.0 * std::pow(4.0, registerPosition) *
            voice.key->hammerStiffness * voice.key->hammerStiffnessScale *
            std::pow(normalizedVelocity, voice.key->velocityBrightnessExponent);
        voice.hammer.position = voice.displacement - 0.004;
        voice.hammer.velocity = hammerSpeed(velocity);
        voice.hammer.mass = 0.012 * std::pow(0.5, registerPosition);
        voice.hammer.active = true;
        // Velocity EQ (the excitation-filter fallback in the research notes): +-8 dB per doubling
        // of velocity above 1 kHz, flat at mf. The hammer alone brightened 7.4 dB too little from
        // pp to ff, and making it stiffen with velocity gave erratic per-note spectra.
        voice.velocityShelf = highShelf(std::min(1000.0, 0.45 * sampleRate),
                                        8.0 * parameters[parameterIndex(ParameterId::velocityBrightness)] *
                                            std::log2(std::max(velocity, 0.01) / referenceVelocity),
                                        sampleRate);
    }

    void release(Voice& voice) {
        if (voice.key != nullptr && voice.key->hasDamper) {
            voice.released = true;
            voice.heldByPedal = false;
        }
    }

    static double radiated(Voice& voice, double input, std::size_t channel) {
        const double filtered = biquad(voice.key->bridgeHighpass, &voice.highpassState[channel * 4], input);
        return biquad(voice.velocityShelf, &voice.shelfState[channel * 4], filtered);
    }

    struct VoiceSample {
        double left = 0.0;   // radiated: bridge high-pass and velocity EQ
        double right = 0.0;
        double bridge = 0.0; // unfiltered: the bridge still feels the hammer's push, which drives
                             // the board and the sympathetic strings even though it radiates little
        double knock = 0.0;  // hammer knock on the board
    };

    VoiceSample processVoice(Voice& voice) {
        const auto& key = *voice.key;
        const double hardness = parameters[parameterIndex(ParameterId::hammerHardness)];
        double force = 0.0;
        if (voice.hammer.active) {
            const double compression = voice.hammer.position - voice.displacement;
            if (compression > 0.0) {
                force = std::min(120.0, voice.stiffness * hardness *
                    std::pow(compression, 1.55));
            }
            voice.hammer.velocity -= force / voice.hammer.mass / sampleRate;
            voice.hammer.position += voice.hammer.velocity / sampleRate;
            const double maximumContactTime = 0.06;
            if (voice.age > static_cast<std::uint64_t>(maximumContactTime * sampleRate) ||
                (voice.hammer.velocity < 0.0 && compression < -0.001)) {
                voice.hammer.active = false;
            }
        }

        if (voice.released) {
            voice.releaseBlend = std::min(1.0, voice.releaseBlend + 40.0 / sampleRate);
        }
        double bridge = 0.0;
        double bridgeRight = 0.0;
        double displacement = 0.0;
        for (std::size_t index = 0; index < key.modeCount; ++index) {
            const auto& coefficients = key.modes[index];
            auto& state = voice.modes[index];
            double a1 = coefficients.sustainA1;
            double a2 = coefficients.sustainA2;
            if (voice.released) {
                a1 += (coefficients.releaseA1 - a1) * voice.releaseBlend;
                a2 += (coefficients.releaseA2 - a2) * voice.releaseBlend;
            }
            const double output = force * coefficients.excitation + a1 * state.y1 - a2 * state.y2;
            state.y2 = state.y1;
            state.y1 = std::abs(output) < 1.0e-30 ? 0.0 : output;
            bridge += state.y1 * coefficients.output;
            bridgeRight += state.y1 * coefficients.outputRight + state.y2 * coefficients.outputRightPrevious;
            if (!coefficients.secondary) {
                displacement += state.y1;
            }
        }
        voice.displacement = std::clamp(displacement * 0.012, -0.02, 0.02);
        ++voice.age;

        if (voice.choking) {
            voice.chokeGain *= 0.9995;
            if (voice.chokeGain < 0.0001) {
                voice.active = false;
                return {};
            }
        }

        const double gain = voice.velocityGain * voice.chokeGain;
        // The impact also knocks the board through the action and frame; scaled by force alone, not
        // by the key level law. Fitted to the recordings' broadband energy 20-100 ms after the
        // strike, which was 8 dB short.
        const double knock = force * voice.chokeGain * 0.0267 *
            parameters[parameterIndex(ParameterId::hammerNoise)];
        const VoiceSample sample{
            radiated(voice, bridge, 0) * gain, radiated(voice, bridgeRight, 1) * gain, bridge * gain, knock};
        voice.level = std::max({std::abs(sample.left), std::abs(sample.right), voice.level * 0.99995});
        if (!voice.hammer.active && voice.level < 1.0e-8 && voice.age > sampleRate) {
            voice.active = false;
        }
        return sample;
    }
};

PianoEngine::PianoEngine() : impl_(std::make_unique<Impl>()) {}
PianoEngine::~PianoEngine() = default;

void PianoEngine::prepare(double sampleRate) {
    impl_->model = std::make_unique<InstrumentModel>(sampleRate);
    impl_->sampleRate = sampleRate;
    impl_->highpassCoefficient = 1.0 / (1.0 + 2.0 * 3.141592653589793 * 100.0 / sampleRate);
    impl_->soundboard.prepare(sampleRate);
    impl_->sympathetic.prepare(sampleRate, *impl_->model);
    impl_->applyMicPosition();
    impl_->voicingDirty = true;
    impl_->resetVoices();
}

void PianoEngine::reset() {
    impl_->resetVoices();
}

void PianoEngine::noteOn(const NoteAddress& address, double velocity) {
    if (impl_->model == nullptr || address.key < 21 || address.key > 108 || address.channel < 0 ||
        address.channel > 15 || !std::isfinite(velocity)) {
        return;
    }
    velocity = std::clamp(velocity, 0.0, 1.0);
    if (velocity == 0.0) {
        velocity = 0.002;
    }
    Voice* voice = impl_->findRestrike(address);
    if (voice == nullptr) {
        voice = impl_->allocateVoice();
    }
    impl_->strike(*voice, address, velocity);
    impl_->updateUndamped();
}

void PianoEngine::noteOff(const NoteAddress& address, double velocity) {
    (void)velocity;
    for (auto& voice : impl_->voices) {
        if (!Impl::matches(voice, address)) {
            continue;
        }
        voice.keyDown = false;
        if (address.channel >= 0 && address.channel < 16 && impl_->sustain[address.channel]) {
            voice.heldByPedal = voice.key->hasDamper;
        } else {
            impl_->release(voice);
        }
    }
    impl_->updateUndamped();
}

void PianoEngine::choke(const NoteAddress& address) {
    for (auto& voice : impl_->voices) {
        if (Impl::matches(voice, address)) {
            voice.choking = true;
            voice.keyDown = false;
            voice.heldByPedal = false;
        }
    }
    impl_->updateUndamped();
}

void PianoEngine::setSustain(std::int16_t channel, bool engaged) {
    if (channel < 0 || channel > 15) {
        return;
    }
    impl_->sustain[channel] = engaged;
    if (!engaged) {
        for (auto& voice : impl_->voices) {
            if (voice.active && voice.address.channel == channel && voice.heldByPedal && !voice.keyDown) {
                impl_->release(voice);
            }
        }
    }
    impl_->updateUndamped();
}

void PianoEngine::setParameter(ParameterId id, double value) {
    if (!std::isfinite(value)) {
        return;
    }
    switch (id) {
        case ParameterId::outputGain:
            value = std::clamp(value, 0.0, 2.0);
            break;
        case ParameterId::hammerHardness:
            value = std::clamp(value, 0.25, 2.0);
            break;
        case ParameterId::sympatheticLevel:
            value = std::clamp(value, 0.0, 1.0);
            break;
        case ParameterId::bodyLevel:
            value = std::clamp(value, 0.0, 2.0);
            break;
        case ParameterId::micPosition:
            value = std::clamp(std::round(value), 0.0, 2.0);
            break;
        case ParameterId::stereoWidth:
        case ParameterId::dynamicRange:
        case ParameterId::velocityBrightness:
        case ParameterId::aftersound:
        case ParameterId::hammerNoise:
            value = std::clamp(value, 0.0, 2.0);
            break;
        case ParameterId::sustainLength:
        case ParameterId::damperRelease:
            value = std::clamp(value, 0.25, 4.0);
            break;
        case ParameterId::unisonDetune:
            value = std::clamp(value, 0.0, 3.0);
            break;
    }
    const bool voicing = id == ParameterId::sustainLength || id == ParameterId::damperRelease ||
        id == ParameterId::unisonDetune || id == ParameterId::aftersound;
    if (voicing && value != impl_->parameters[parameterIndex(id)]) {
        impl_->voicingDirty = true;
    }
    impl_->parameters[parameterIndex(id)] = value;
    // Hosts resend every parameter on any change, so only recompute coefficients on a real
    // mic change. Switching mid-note can click; it is a setup choice, not a performance one.
    if (id == ParameterId::micPosition && impl_->model != nullptr &&
        static_cast<int>(value) != static_cast<int>(impl_->micPosition)) {
        impl_->applyMicPosition();
    }
}

double PianoEngine::parameterValue(ParameterId id) const {
    return impl_->parameters[parameterIndex(id)];
}

void PianoEngine::render(float* left, float* right, std::uint32_t frameCount) {
    if (left == nullptr || right == nullptr) {
        return;
    }
    std::fill_n(left, frameCount, 0.0f);
    std::fill_n(right, frameCount, 0.0f);
    if (impl_->model == nullptr) {
        return;
    }
    if (impl_->voicingDirty) {
        impl_->applyVoicing();
    }

    const double targetGain = impl_->parameters[parameterIndex(ParameterId::outputGain)];
    const double sympatheticLevel = impl_->parameters[parameterIndex(ParameterId::sympatheticLevel)];
    const double bodyLevel = impl_->parameters[parameterIndex(ParameterId::bodyLevel)];
    const double targetWidth = impl_->parameters[parameterIndex(ParameterId::stereoWidth)];
    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        std::array<double, 3> regionLeft{};
        std::array<double, 3> regionRight{};
        std::array<double, 3> regionMono{};
        std::array<double, 3> regionSympathetic{};
        std::array<double, 3> regionKnock{};
        for (auto& voice : impl_->voices) {
            if (!voice.active || voice.key == nullptr) {
                continue;
            }
            const std::size_t region = voice.address.key < 48 ? 0 : (voice.address.key < 72 ? 1 : 2);
            const auto sample = impl_->processVoice(voice);
            const double sampleLeft = sample.left * voice.key->leftGain;
            const double sampleRight = sample.right * voice.key->rightGain;
            regionLeft[region] += sampleLeft;
            regionRight[region] += sampleRight;
            // Sympathetic strings hear the bridge, not the per-channel radiation phase.
            const double bridgeSample = 0.5 * sample.bridge * (voice.key->leftGain + voice.key->rightGain);
            regionMono[region] += bridgeSample;
            regionSympathetic[region] += bridgeSample * voice.key->sympatheticDrive;
            regionKnock[region] += sample.knock;
        }
        const auto resonance = impl_->sympathetic.process(regionSympathetic, impl_->undamped, sympatheticLevel);
        std::array<double, 3> bridge{};
        for (std::size_t region = 0; region < 3; ++region) {
            regionLeft[region] += resonance[region] * 0.707;
            regionRight[region] += resonance[region] * 0.707;
            bridge[region] = regionMono[region] + resonance[region] + regionKnock[region];
        }
        double mixedLeft = 0.0;
        double mixedRight = 0.0;
        impl_->soundboard.process(regionLeft, regionRight, bridge, bodyLevel, mixedLeft, mixedRight);
        impl_->smoothedGain += 0.0002 * (targetGain - impl_->smoothedGain);
        impl_->smoothedWidth += 0.0002 * (targetWidth - impl_->smoothedWidth);
        const double filteredLeft = impl_->highpass(mixedLeft, 0);
        const double filteredRight = impl_->highpass(mixedRight, 1);
        const double mid = 0.5 * (filteredLeft + filteredRight);
        const double side = 0.5 * (filteredLeft - filteredRight) * impl_->smoothedWidth;
        const double outputLeft = (mid + side) * impl_->smoothedGain;
        const double outputRight = (mid - side) * impl_->smoothedGain;
        left[frame] = std::abs(outputLeft) < 1.0e-20 ? 0.0F : static_cast<float>(outputLeft);
        right[frame] = std::abs(outputRight) < 1.0e-20 ? 0.0F : static_cast<float>(outputRight);
    }
}

std::size_t PianoEngine::activeVoiceCount() const {
    return static_cast<std::size_t>(std::count_if(impl_->voices.begin(), impl_->voices.end(),
        [](const Voice& voice) { return voice.active; }));
}

} // namespace pianocraft
