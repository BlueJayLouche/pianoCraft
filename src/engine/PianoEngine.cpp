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

constexpr std::size_t voiceCount = 64;
constexpr std::size_t parameterCount = 5;

std::size_t parameterIndex(ParameterId id) {
    return static_cast<std::size_t>(id) < parameterCount ? static_cast<std::size_t>(id) : 0;
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

    Impl() {
        parameters.fill(0.0);
        parameters[parameterIndex(ParameterId::outputGain)] = 0.7;
        parameters[parameterIndex(ParameterId::hammerHardness)] = 1.0;
        parameters[parameterIndex(ParameterId::sympatheticLevel)] = 0.35;
        parameters[parameterIndex(ParameterId::bodyLevel)] = 1.0;
    }

    void resetVoices() {
        for (auto& voice : voices) {
            voice.clear();
        }
        sustain.fill(false);
        undamped.fill(false);
        soundboard.reset();
        sympathetic.reset();
        smoothedGain = parameters[parameterIndex(ParameterId::outputGain)];
    }

    void updateUndamped() {
        if (std::any_of(sustain.begin(), sustain.end(), [](bool engaged) { return engaged; })) {
            undamped.fill(true);
            return;
        }
        undamped.fill(false);
        for (const auto& voice : voices) {
            if (voice.active && voice.key != nullptr &&
                (voice.keyDown || !voice.key->hasDamper)) {
                undamped[voice.address.key - 21] = true;
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
        voice.velocityGain = referenceGain * std::pow(normalizedVelocity, voice.key->velocityGainExponent) *
            voice.key->outputGain;
        voice.stiffness = voice.key->hammerStiffness * voice.key->hammerStiffnessScale *
            std::pow(normalizedVelocity, voice.key->velocityBrightnessExponent);
        voice.hammer.position = voice.displacement - 0.004;
        voice.hammer.velocity = 0.35 + 2.4 * std::pow(velocity, 0.85);
        voice.hammer.mass = 0.018;
        voice.hammer.active = true;
    }

    void release(Voice& voice) {
        if (voice.key != nullptr && voice.key->hasDamper) {
            voice.released = true;
            voice.heldByPedal = false;
        }
    }

    double processVoice(Voice& voice) {
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
                return 0.0;
            }
        }

        const double sample = bridge * voice.velocityGain * voice.chokeGain;
        voice.level = std::max(std::abs(sample), voice.level * 0.99995);
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
    impl_->soundboard.prepare(sampleRate);
    impl_->sympathetic.prepare(sampleRate, *impl_->model);
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
    }
    impl_->parameters[parameterIndex(id)] = value;
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

    const double targetGain = impl_->parameters[parameterIndex(ParameterId::outputGain)];
    const double sympatheticLevel = impl_->parameters[parameterIndex(ParameterId::sympatheticLevel)];
    const double bodyLevel = impl_->parameters[parameterIndex(ParameterId::bodyLevel)];
    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        std::array<double, 3> regionLeft{};
        std::array<double, 3> regionRight{};
        std::array<double, 3> regionMono{};
        for (auto& voice : impl_->voices) {
            if (!voice.active || voice.key == nullptr) {
                continue;
            }
            const std::size_t region = voice.address.key < 48 ? 0 : (voice.address.key < 72 ? 1 : 2);
            const double sample = impl_->processVoice(voice);
            const double sampleLeft = sample * voice.key->leftGain;
            const double sampleRight = sample * voice.key->rightGain;
            regionLeft[region] += sampleLeft;
            regionRight[region] += sampleRight;
            regionMono[region] += 0.5 * (sampleLeft + sampleRight);
        }
        const auto resonance = impl_->sympathetic.process(regionMono, impl_->undamped, sympatheticLevel);
        for (std::size_t region = 0; region < 3; ++region) {
            regionLeft[region] += resonance[region] * 0.707;
            regionRight[region] += resonance[region] * 0.707;
        }
        double mixedLeft = 0.0;
        double mixedRight = 0.0;
        impl_->soundboard.process(regionLeft, regionRight, bodyLevel, mixedLeft, mixedRight);
        impl_->smoothedGain += 0.0002 * (targetGain - impl_->smoothedGain);
        const double outputLeft = mixedLeft * impl_->smoothedGain;
        const double outputRight = mixedRight * impl_->smoothedGain;
        left[frame] = std::abs(outputLeft) < 1.0e-20 ? 0.0F : static_cast<float>(outputLeft);
        right[frame] = std::abs(outputRight) < 1.0e-20 ? 0.0F : static_cast<float>(outputRight);
    }
}

std::size_t PianoEngine::activeVoiceCount() const {
    return static_cast<std::size_t>(std::count_if(impl_->voices.begin(), impl_->voices.end(),
        [](const Voice& voice) { return voice.active; }));
}

} // namespace pianocraft
