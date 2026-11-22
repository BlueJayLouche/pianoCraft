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
#include <utility>

namespace pianocraft {
namespace {

constexpr std::size_t voiceCount = voiceCapacity;
constexpr std::size_t parameterCount = 18;

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
    double mass = 0.008;
    double load = 0.0; // last felt compression^p, for the hysteresis term
    bool active = false;
};

struct Voice {
    const KeyModel* key = nullptr;
    NoteAddress address{};
    std::array<ModeState, modeLimit> modes{};
    // While the hammer is on, the part of each mode's motion this strike put there: what the hammer meets.
    // A restruck string's ringing is rescaled to the new strike's gain (strike()), up to 100x for a soft
    // strike on a loud note, and read through the new strike's shares; met by the hammer, it threw it hard
    // enough to peak 36 dB over the loud note, and repeated restrikes grew without bound. The hammer now
    // lands as on a still string; the ringing sounds on, untouched by it.
    std::array<ModeState, modeLimit> struckModes{};
    std::array<ModeState, longitudinalLimit> longitudinal{};
    HammerState hammer{};
    std::uint64_t age = 0;
    double displacement = 0.0; // the string under the hammer next sample if the force stopped (m)
    double level = 0.0;
    double velocityGain = 1.0;
    double knockGain = 1.0;
    double releaseBlend = 0.0;
    double residualBlend = 0.0; // the damped string's second stage (KeyModel::residualDelay)
    std::uint64_t damped = 0;   // samples the dampers have been seated
    double chokeGain = 1.0;
    bool active = false;
    bool keyDown = false;
    bool choking = false;
    std::array<double, 8> highpassState{};
    // The strike's share of each mode, set by its velocity (tools/fit_piano.py): an excitation filter. It
    // shapes only what the hammer puts in, so a restruck string's ringing keeps its own spectrum and nothing
    // steps. The hammer itself meets the physical string: it reads each mode through the inverse share.
    std::array<double, modeLimit> weight{};
    std::array<double, modeLimit> unweight{};
    double strikeOctaves = 0.0; // the strike's velocity, octaves re mf
    std::array<double, 5> knockShelf{1.0, 0.0, 0.0, 0.0, 0.0};
    // The phantom partials' drive is the string's motion squared, so a restrike's rescaled ringing
    // takes it with a matching scale; it glides to whichever strike now dominates.
    double phantomScale = 1.0;
    double phantomTarget = 1.0;
    std::array<double, 4> knockState{};
    std::array<double, 2> knockLow{};
    double knockLowpass = 1.0;
    // Per-note CLAP modulation and expressions.
    double hardnessModulation = 0.0;
    double brightness = 0.0; // Timbre's share of the hardness offset
    double noiseModulation = 0.0;
    double volume = 1.0;
    double volumeTarget = 1.0;
    double panLeft = 1.0;
    double panRight = 1.0;
    double panLeftTarget = 1.0;
    double panRightTarget = 1.0;

    void neutralExpressions() {
        hardnessModulation = 0.0;
        brightness = 0.0;
        noiseModulation = 0.0;
        volumeTarget = 1.0;
        panLeftTarget = 1.0;
        panRightTarget = 1.0;
    }

    void clear() {
        key = nullptr;
        address = {};
        modes = {};
        struckModes = {};
        longitudinal = {};
        hammer = {};
        age = 0;
        displacement = 0.0;
        level = 0.0;
        velocityGain = 1.0;
        knockGain = 1.0;
        releaseBlend = 0.0;
        residualBlend = 0.0;
        damped = 0;
        chokeGain = 1.0;
        active = false;
        keyDown = false;
        choking = false;
        highpassState = {};
        weight = {};
        unweight = {};
        knockShelf = {1.0, 0.0, 0.0, 0.0, 0.0};
        phantomScale = phantomTarget = 1.0;
        knockState = {};
        knockLow = {};
        knockLowpass = 1.0;
        neutralExpressions();
        volume = 1.0;
        panLeft = 1.0;
        panRight = 1.0;
    }
};

} // namespace

struct PianoEngine::Impl {
    std::unique_ptr<InstrumentModel> model;
    std::array<Voice, voiceCount> voices{};
    // Damper contact per channel: 1 with the pedal up, 0 with it down.
    std::array<double, 16> damperContact{};
    std::array<double, 16> softPedal{};
    // The sustain pedal as a moving part: the pedal, trapwork and damper rail follow the pedal's
    // depth as one mass, critically damped, a full stroke in ~25 ms. The dampers ride the rail, and
    // its speed is what sounds.
    std::array<double, 16> pedalTarget{};
    std::array<double, 16> pedalPosition{};
    std::array<double, 16> pedalSpeed{};
    double pedalOmega = 1.0;
    double pedalThumpLowpass = 1.0;
    double pedalThump = 0.0; // the knock's low-pass state
    std::uint32_t pedalNoiseState = 0x6C8E9CF5u;
    // How free each string is to ring sympathetically, 0 (damped) to 1.
    std::array<double, instrumentKeyCount> undamped{};
    std::array<double, parameterCount> parameters{};
    Soundboard soundboard;
    SympatheticBank sympathetic;
    double sampleRate = 48000.0;
    double smoothedGain = 0.7;
    double smoothedWidth = 1.0;
    MicPosition micPosition = MicPosition::spacedPair;
    bool voicingDirty = true;
    std::array<double, parameterCount> modulation{}; // CLAP global (monophonic) modulation
    std::array<EndedNote, 2 * voiceCount> endedNotes{};
    std::size_t endedNoteCount = 0;
    std::uint32_t currentFrame = 0;

    void endNote(const NoteAddress& address) {
        // ponytail: drops ends past capacity when nobody drains them (offline tools, tests).
        if (endedNoteCount < endedNotes.size()) {
            endedNotes[endedNoteCount++] = {address, currentFrame};
        }
    }
    // ponytail: three one-pole high-passes at 100 Hz stand in for the soundboard's weak
    // radiation of low fundamentals; replace with a radiation model if the bass needs more.
    static constexpr std::size_t highpassStages = 3;
    std::array<double, 2 * highpassStages> highpassInput{};
    std::array<double, 2 * highpassStages> highpassOutput{};
    double highpassCoefficient = 1.0;
    double phantomGlide = 1.0;  // and its phantom drive moves over 1 ms, before the hammer lands

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
                        ParameterId::hammerNoise, ParameterId::pedalNoise}) {
            parameters[parameterIndex(id)] = 1.0;
        }
        parameters[parameterIndex(ParameterId::masterTune)] = 440.0;
        parameters[parameterIndex(ParameterId::stretch)] = 1.0;
    }

    // ponytail: recomputed on the audio thread at the next render() after a change, at most once
    // per block: ~65 us, worst ~200 us at 48 kHz. Move off-thread if fast voicing automation glitches.
    void applyVoicing() {
        const auto value = [this](ParameterId id) { return parameters[parameterIndex(id)]; };
        Voicing voicing;
        voicing.sustain = value(ParameterId::sustainLength);
        voicing.release = value(ParameterId::damperRelease);
        voicing.detune = value(ParameterId::unisonDetune);
        voicing.aftersound = value(ParameterId::aftersound);
        voicing.masterTune = value(ParameterId::masterTune);
        voicing.stretch = value(ParameterId::stretch);
        voicing.age = value(ParameterId::age);
        model->setVoicing(voicing);
        sympathetic.update(sampleRate, *model);
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
        damperContact.fill(1.0);
        softPedal.fill(0.0);
        pedalTarget.fill(0.0);
        pedalPosition.fill(0.0);
        pedalSpeed.fill(0.0);
        pedalThump = 0.0;
        undamped.fill(0.0);
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

    // Strings free to ring sympathetically: as far as the most-lifted pedal allows, and the
    // damperless top keys always. A string already sounding as a voice is excluded so it is not doubled.
    // The pedals' motion this sample, and their noise: the trapwork's knock, for the board, and the damper felt
    // brushing the strings as it leaves or lands on them, for the strings. Both follow the rail's
    // speed, so a stamp (or square automation) is loud, a slow or half pedal quiet, and nothing
    // starts in a sample. Analytic: the corpus has no pedal noise, so the levels are chosen: a stamp
    // landing (the dampers dropping onto the strings) 39 dB under a mf C4's peak, lifting 49 dB.
    // Analytic half-pedal zone: dampers fully on below 25% travel, fully off above 75%. Squared
    // because damping shortens decay fast: mid-travel then leaves C3 ringing ~1.5 s rather than
    // ~0.8 s. The corpus has no part-pedalled notes, so this is chosen, not measured.
    static double damperContactAt(double depth) {
        const double contact = std::clamp((0.75 - depth) / 0.5, 0.0, 1.0);
        return contact * contact;
    }

    std::pair<double, double> movePedals() {
        double speed = 0.0;
        bool moved = false;
        for (std::size_t channel = 0; channel < pedalTarget.size(); ++channel) {
            double& position = pedalPosition[channel];
            double& velocity = pedalSpeed[channel];
            if (velocity == 0.0 && position == pedalTarget[channel]) {
                continue;
            }
            velocity += (pedalOmega * pedalOmega * (pedalTarget[channel] - position) - 2.0 * pedalOmega * velocity) / sampleRate;
            position += velocity / sampleRate;
            if (std::abs(velocity) < 1.0e-6 && std::abs(pedalTarget[channel] - position) < 1.0e-7) {
                velocity = 0.0;
                position = pedalTarget[channel];
            }
            speed += std::abs(velocity) * (velocity < 0.0 ? 1.0 : 0.6);
            const double contact = damperContactAt(position);
            moved = moved || contact != damperContact[channel];
            damperContact[channel] = contact;
        }
        if (moved) {
            updateUndamped();
        }
        const double level = parameters[parameterIndex(ParameterId::pedalNoise)] *
            (1.0 + parameters[parameterIndex(ParameterId::age)]); // worn trapwork rattles more
        if (speed == 0.0 && pedalThump == 0.0) {
            return {0.0, 0.0};
        }
        // A full stroke's peak speed is omega / e: that stamp is 1.
        const double drive = level * speed * 2.718281828459045 / pedalOmega;
        pedalNoiseState ^= pedalNoiseState << 13;
        pedalNoiseState ^= pedalNoiseState >> 17;
        pedalNoiseState ^= pedalNoiseState << 5;
        const double noise = static_cast<double>(pedalNoiseState) / 2147483648.0 - 1.0;
        // Wood on felt: a dull knock, below ~400 Hz.
        pedalThump += pedalThumpLowpass * (drive * noise - pedalThump);
        if (std::abs(pedalThump) < 1.0e-20) {
            pedalThump = 0.0;
        }
        return {pedalThump * pedalKnockLevel, drive * noise * pedalBrushLevel};
    }
    // ponytail: chosen, not fitted (see pedalNoise); refit if licensed pedal recordings turn up.
    // 20 dB under the first choice, which sounded far too loud by ear (20 Nov 2026).
    static constexpr double pedalKnockLevel = 0.005;
    static constexpr double pedalBrushLevel = 0.02;

    void updateUndamped() {
        const double lifted = 1.0 - *std::min_element(damperContact.begin(), damperContact.end());
        for (std::size_t index = 0; index < undamped.size(); ++index) {
            undamped[index] = model->key(static_cast<int>(index) + 21).hasDamper ? lifted : 1.0;
        }
        for (const auto& voice : voices) {
            if (voice.active && voice.key != nullptr) {
                undamped[voice.address.key - 21] = 0.0;
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

    // CLAP targeting: a negative note id, port, channel or key matches any.
    static bool targets(const Voice& voice, const NoteAddress& target) {
        const auto fits = [](int wanted, int actual) { return wanted < 0 || wanted == actual; };
        return voice.active && voice.key != nullptr && fits(target.noteId, voice.address.noteId) &&
            fits(target.port, voice.address.port) && fits(target.channel, voice.address.channel) &&
            fits(target.key, voice.address.key);
    }

    // The same string, whatever note id struck it last: hosts give every note a fresh id.
    Voice* findRestrike(const NoteAddress& address) {
        Voice* candidate = nullptr;
        for (auto& voice : voices) {
            if (targets(voice, {-1, address.port, address.channel, address.key}) && !voice.keyDown &&
                !voice.choking) {
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
            // Held keys matter most, then damperless and pedal-held strings, then damped ones.
            double score = voice.level;
            if (voice.keyDown) {
                score *= 12.0;
            } else if (!voice.key->hasDamper) {
                score *= 4.0;
            } else {
                score *= 1.0 + 7.0 * (1.0 - damperContact[voice.address.channel]);
            }
            if (score < bestScore || (score == bestScore && voice.age > selected->age)) {
                selected = &voice;
                bestScore = score;
            }
        }
        endNote(selected->address);
        selected->clear();
        return selected;
    }

    // Velocity shapes the strike's spectrum (the excitation filter of the research notes): each mode's share
    // follows its fitted law in dB, linear in log velocity through 40, 80 and 120 (of 127) and held below 20.
    // The hammer alone brightens too little from pp to ff: in the mid register the string's give, not the felt,
    // sets the contact time. Velocity Brightness scales the recordings' own brightening against the fundamental,
    // leaving its level: 0 plays every velocity with mf's spectrum. Hammer Hardness (with Timbre) moves along
    // the same brightening: hardness h takes the spectrum of log2(h) octaves more velocity at the same level,
    // as the felt's stiffness also changes (feltForce). Set at the strike, and again by modulation or Timbre
    // that arrives with it, before the voice has sounded.
    void shape(Voice& voice) {
        const auto& key = *voice.key;
        if (key.modeCount == 0) {
            return;
        }
        const double brightness = parameters[parameterIndex(ParameterId::velocityBrightness)];
        const std::size_t hardnessIndex = parameterIndex(ParameterId::hammerHardness);
        const double hardness = std::clamp(parameters[hardnessIndex] + modulation[hardnessIndex] +
                                           voice.hardnessModulation + voice.brightness, 0.25, 2.0);
        const double x = voice.strikeOctaves;
        const double harder = std::clamp(x + std::log2(hardness), -2.0, 1.0);
        const auto atVelocity = [](double at, double soft, double loud) {
            at = std::max(at, -2.0);
            return at < 0.0 ? -at * soft : at / std::log2(1.5) * loud;
        };
        const auto tilt = [&](const ModeCoefficients& mode, double at) {
            return atVelocity(at, mode.softBrighteningDb, mode.loudBrighteningDb) -
                atVelocity(at, key.modes[0].softBrighteningDb, key.modes[0].loudBrighteningDb);
        };
        // Read through 1 / share (at most 40 dB up), the modes give under the hammer as the physical string does:
        // a boosted share once made the string livelier than it is, and ff unstable at very low sample rates.
        for (std::size_t index = 0; index < key.modeCount; ++index) {
            const auto& mode = key.modes[index];
            const double db = atVelocity(x, mode.softDb, mode.loudDb) + (brightness - 1.0) * tilt(mode, x) +
                tilt(mode, harder) - tilt(mode, x);
            // At most +30 dB, the most the recordings ask (velocity 40, the softest, 5-9 kHz). Unbounded, the soft
            // law's extrapolation to velocity 20, Velocity Brightness 2 or Hammer Hardness 0.25 took a treble
            // note's top partials to +117 dB and its peak to +39 dBFS.
            voice.weight[index] = std::pow(10.0, std::clamp(db, -90.0, 30.0) / 20.0);
            voice.unweight[index] = 1.0 / std::max(voice.weight[index], 0.01);
        }
    }

    void strike(Voice& voice, const NoteAddress& address, double velocity) {
        constexpr double referenceVelocity = 80.0 / 127.0;
        const bool ringing = voice.key != nullptr; // restruck, not a fresh voice
        voice.key = &model->key(address.key);
        voice.address = address;
        voice.active = true;
        voice.keyDown = true;
        voice.choking = false;
        voice.chokeGain = 1.0;
        voice.age = 0;
        voice.neutralExpressions();
        voice.strikeOctaves = std::log2(std::max(velocity, 0.01) / referenceVelocity);
        shape(voice);
        const double normalizedVelocity = velocity / referenceVelocity;
        const double referenceGain = 0.18 + 0.82 * std::pow(referenceVelocity, 1.35);
        const auto hammerSpeed = [](double v) { return 0.35 + 2.4 * std::pow(v, 0.85); };
        // Dynamic Range scales each note's dB distance from mf: the gain law's share directly, and
        // the hammer speed's own share through a compensating gain.
        const double dynamics = parameters[parameterIndex(ParameterId::dynamicRange)];
        // The recordings rise 4.5 dB from mf to ff where the full law gave 10 dB; above mf the law
        // keeps a selected share of its slope and the hammer's impulse does the rest.
        const double topSlope = velocity > referenceVelocity ? 0.2 : 1.0;
        const double previousGain = voice.velocityGain;
        voice.velocityGain = referenceGain *
            std::pow(normalizedVelocity, voice.key->velocityGainExponent * topSlope * dynamics) *
            std::pow(hammerSpeed(velocity) / hammerSpeed(referenceVelocity), dynamics - 1.0) *
            voice.key->outputGain * voice.key->wearGain;
        // A restruck string is still ringing, loudly under the pedal: only the new strike takes the
        // new gain, so the ringing and the filters holding it are rescaled to sound as before.
        // Stepped to the new gain, the ringing clicked: 1000x its sharpest bend. A fresh voice has no
        // ringing to carry: taken from clear()'s gain of 1, it scaled every note's phantoms by its own gain.
        const double carry = ringing ? previousGain / voice.velocityGain : 1.0;
        for (auto& mode : voice.modes) {
            mode.y1 *= carry;
            mode.y2 *= carry;
        }
        for (auto& mode : voice.longitudinal) {
            mode.y1 *= carry;
            mode.y2 *= carry;
        }
        for (auto& value : voice.highpassState) {
            value *= carry;
        }
        // Unscaled, the phantoms jumped carry-fold at once. A louder strike's own partials take over
        // (scale 1); a softer one's ringing still dominates and keeps its scale.
        voice.phantomScale /= carry;
        voice.phantomTarget = carry < 1.0 ? 1.0 : voice.phantomScale;
        voice.knockGain = std::pow(hammerSpeed(velocity) / hammerSpeed(referenceVelocity), dynamics - 1.0);
        // A heavy bass hammer thumps the board low and a light treble one knocks higher: two poles at 16 f0, at
        // most 4 kHz. Open above C5, the treble's knock clicked: its notes peaked in 2-4 ms and fell 10-15 dB by
        // 30 ms, where the recordings rise over 10 ms and hold.
        voice.knockLowpass = 1.0 - std::exp(-2.0 * 3.141592653589793 *
            std::min({16.0 * voice.key->fundamental, 4000.0, 0.45 * sampleRate}) / sampleRate);
        voice.struckModes = {};
        voice.hammer.position = voice.displacement - 0.004;
        voice.hammer.velocity = hammerSpeed(velocity);
        voice.hammer.mass = voice.key->hammerMass;
        voice.hammer.load = 0.0;
        voice.hammer.active = true;
        // The knock keeps a plain shelf above 1 kHz, flat at mf: 8 dB per doubling of velocity above
        // mf, 20 dB per halving below; without it a soft note began with a loud one's click.
        const double octaves = voice.strikeOctaves;
        voice.knockShelf = highShelf(std::min(1000.0, 0.45 * sampleRate), (octaves < 0.0 ? 20.0 : 8.0) * octaves *
                                     parameters[parameterIndex(ParameterId::velocityBrightness)], sampleRate);
    }

    static double radiated(Voice& voice, double input, std::size_t channel) {
        return biquad(voice.key->bridgeHighpass, &voice.highpassState[channel * 4], input);
    }

    struct VoiceSample {
        double left = 0.0;   // radiated: through the bridge high-pass
        double right = 0.0;
        double centre = 0.0; // the left channel's signal before key and note panning: mono-safe
        double bridge = 0.0; // unfiltered: the bridge still feels the hammer's push, which drives
                             // the board and the sympathetic strings even though it radiates little
        double knock = 0.0;  // hammer knock on the board
    };

    // Stulov's hereditary felt in its slow-loading form, valid because its relaxation time is
    // microseconds: F = Q0 (u^p + lambda d(u^p)/dt), never pulling. Within the sample the string
    // gives way by C F: the synthesized modes' share and the residual compliance of the rest. Solving
    // u = gap - C F(u) implicitly keeps the light treble strings stable under stiff felt; Newton from
    // u = gap converges from above because u + C F(u) is convex.
    double feltForce(Voice& voice, double hardness) const {
        const auto& key = *voice.key;
        const double gap = voice.hammer.position - voice.displacement;
        const double compliance = key.residualCompliance + key.strikeCompliance;
        // Hammer Hardness divides the felt's compression at a given force: Q0 h^p. In the mid
        // register the string's give sets the contact time, so a linear Q0 scale was nearly inaudible.
        const double p = key.feltExponent;
        const double stiffness = key.feltStiffness * std::pow(hardness * key.wearHardness, p);
        const double memory = key.feltRelaxation * sampleRate;
        const auto force = [&](double u) {
            return u > 0.0 ? std::max(0.0, stiffness * ((1.0 + memory) * std::pow(u, p) - memory * voice.hammer.load))
                           : 0.0;
        };
        double u = gap;
        double f = force(u);
        for (int iteration = 0; iteration < 12 && f > 0.0; ++iteration) {
            const double slope = 1.0 + compliance * stiffness * (1.0 + memory) * p * std::pow(u, p - 1.0);
            const double step = (u + compliance * f - gap) / slope;
            u -= step;
            f = force(u);
            if (std::abs(step) < 1.0e-12) {
                break;
            }
        }
        voice.hammer.load = u > 0.0 ? std::pow(u, p) : 0.0;
        return f;
    }

    VoiceSample processVoice(Voice& voice) {
        const auto& key = *voice.key;
        const std::size_t hardnessIndex = parameterIndex(ParameterId::hammerHardness);
        const double hardness = std::clamp(
            parameters[hardnessIndex] + modulation[hardnessIndex] + voice.hardnessModulation + voice.brightness,
            0.25, 2.0);
        double force = 0.0;
        if (voice.hammer.active) {
            force = feltForce(voice, hardness);
            const double compression = voice.hammer.position - voice.displacement;
            voice.hammer.velocity -= force / voice.hammer.mass / sampleRate;
            voice.hammer.position += voice.hammer.velocity / sampleRate;
            const double maximumContactTime = 0.06;
            if (voice.age > static_cast<std::uint64_t>(maximumContactTime * sampleRate) ||
                (voice.hammer.velocity < 0.0 && compression < -0.001)) {
                voice.hammer.active = false;
            }
        }

        // Dampers rest on a string once its key is up, as far as the pedal lets them, and lift off
        // again when the pedal goes down, catching what is still ringing. Fully on or off in 25 ms.
        const double damping = voice.keyDown || !key.hasDamper ? 0.0 : damperContact[voice.address.channel];
        const double step = 40.0 / sampleRate;
        voice.releaseBlend += std::clamp(damping - voice.releaseBlend, -step, step);
        // Seated (over half on) for the key's delay, the dampers leave the slow residual, gliding in over 25 ms.
        voice.damped = voice.releaseBlend > 0.5 ? voice.damped + 1 : 0;
        const double residualTarget = voice.damped > key.residualDelay * sampleRate ? 1.0 : 0.0;
        voice.residualBlend += std::clamp(residualTarget - voice.residualBlend, -step, step);
        double bridge = 0.0;
        double bridgeRight = 0.0;
        double drive = 0.0;
        double displacement = 0.0;
        const bool struck = voice.hammer.active;
        for (std::size_t index = 0; index < key.modeCount; ++index) {
            const auto& coefficients = key.modes[index];
            auto& state = voice.modes[index];
            double a1 = coefficients.sustainA1;
            double a2 = coefficients.sustainA2;
            if (voice.releaseBlend > 0.0) {
                double releaseA1 = coefficients.releaseA1;
                double releaseA2 = coefficients.releaseA2;
                if (voice.residualBlend > 0.0) {
                    releaseA1 += (coefficients.residualA1 - releaseA1) * voice.residualBlend;
                    releaseA2 += (coefficients.residualA2 - releaseA2) * voice.residualBlend;
                }
                a1 += (releaseA1 - a1) * voice.releaseBlend;
                a2 += (releaseA2 - a2) * voice.releaseBlend;
            }
            const double input = force * coefficients.excitation * voice.weight[index];
            const double output = input + a1 * state.y1 - a2 * state.y2;
            state.y2 = state.y1;
            state.y1 = std::abs(output) < 1.0e-30 ? 0.0 : output;
            bridge += state.y1 * coefficients.outputLeft + state.y2 * coefficients.outputLeftPrevious;
            drive += state.y1 * coefficients.driveCurrent + state.y2 * coefficients.drivePrevious;
            bridgeRight += state.y1 * coefficients.outputRight + state.y2 * coefficients.outputRightPrevious;
            if (struck) {
                auto& own = voice.struckModes[index];
                const double moved = input + a1 * own.y1 - a2 * own.y2;
                own.y2 = own.y1;
                own.y1 = std::abs(moved) < 1.0e-30 ? 0.0 : moved;
                displacement += coefficients.strike * voice.unweight[index] * (a1 * own.y1 - a2 * own.y2);
            }
        }
        voice.displacement = displacement;
        ++voice.age;

        // Phantom partials: the squared string slope drives the longitudinal modes, whose force
        // on the bridge carries sums of partial frequencies (Bank & Sujbert).
        // ponytail: neighbouring primaries stand for partials k, k+1; a calibrated partial missing
        // from the series pairs non-neighbours there. Pair by partial number if that is audible.
        if (key.longitudinalCount > 0) {
            double adjacent = 0.0;
            double skip = 0.0;
            double previous = 0.0;
            double beforePrevious = 0.0;
            for (std::size_t index = 0; index < key.primaryModeCount; ++index) {
                const double slope = voice.modes[index].y1 * key.modes[index].slope;
                adjacent += slope * previous;
                skip += slope * beforePrevious;
                beforePrevious = previous;
                previous = slope;
            }
            const double first = voice.modes[0].y1 * key.modes[0].slope;
            voice.phantomScale += phantomGlide * (voice.phantomTarget - voice.phantomScale);
            const double excitation =
                2.0 * key.longitudinalDrive * voice.phantomScale * (adjacent + first * first + 2.0 * skip);
            double longitudinalForce = 0.0;
            double longitudinalDrive = 0.0;
            for (std::size_t index = 0; index < key.longitudinalCount; ++index) {
                const auto& coefficients = key.longitudinal[index];
                auto& state = voice.longitudinal[index];
                const double output = excitation * coefficients.input + coefficients.a1 * state.y1 - coefficients.a2 * state.y2;
                state.y2 = state.y1;
                state.y1 = std::abs(output) < 1.0e-30 ? 0.0 : output;
                longitudinalForce += state.y1 * coefficients.output;
                longitudinalDrive += state.y1 * coefficients.drive;
            }
            bridge += longitudinalForce;
            bridgeRight += longitudinalForce;
            drive += longitudinalDrive;
        }

        if (voice.choking) {
            voice.chokeGain *= 0.9995;
            if (voice.chokeGain < 0.0001) {
                voice.active = false;
                endNote(voice.address);
                return {};
            }
        }

        // Note expressions glide over ~10 ms so automation doesn't zipper.
        const double glide = 100.0 / sampleRate;
        voice.volume += glide * (voice.volumeTarget - voice.volume);
        voice.panLeft += glide * (voice.panLeftTarget - voice.panLeft);
        voice.panRight += glide * (voice.panRightTarget - voice.panRight);
        const double gain = voice.velocityGain * voice.chokeGain * voice.volume;
        // The impact also knocks the board through the action and frame; scaled by force alone, not
        // by the key level law. Fitted to the recordings' broadband energy 20-100 ms after the
        // strike, which was 8 dB short. It is the same force, so it takes the strings' velocity EQ:
        // without it a soft note began with a click as bright as a loud one's.
        const std::size_t noiseIndex = parameterIndex(ParameterId::hammerNoise);
        const double noise =
            std::clamp(parameters[noiseIndex] + modulation[noiseIndex] + voice.noiseModulation, 0.0, 2.0);
        const double knock = biquad(voice.knockShelf, voice.knockState.data(),
                                    force * voice.knockGain * voice.chokeGain * voice.volume * 0.03 * noise);
        voice.knockLow[0] += voice.knockLowpass * (knock - voice.knockLow[0]);
        voice.knockLow[1] += voice.knockLowpass * (voice.knockLow[0] - voice.knockLow[1]);
        const double radiatedLeft = radiated(voice, bridge, 0) * gain;
        const VoiceSample sample{radiatedLeft * voice.panLeft, radiated(voice, bridgeRight, 1) * gain * voice.panRight,
                                 radiatedLeft * 0.5 * (voice.panLeft + voice.panRight), drive * gain, voice.knockLow[1]};
        voice.level = std::max({std::abs(sample.left), std::abs(sample.right), voice.level * 0.99995});
        if (!voice.hammer.active && voice.level < 1.0e-8 && voice.age > sampleRate) {
            voice.active = false;
            endNote(voice.address);
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
    impl_->phantomGlide = 1.0 - std::exp(-1.0 / (0.001 * sampleRate));
    impl_->pedalOmega = 1.0 / 0.006; // peak speed 6 ms in, 90% there by 24 ms
    impl_->pedalThumpLowpass = 1.0 - std::exp(-2.0 * 3.141592653589793 * 400.0 / sampleRate);
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
    // An upright's soft pedal moves the hammers toward the strings; the shorter throw lands them
    // slower, so the note is both softer and darker. Analytic: the corpus has no soft-pedal notes.
    velocity *= 1.0 - 0.3 * impl_->softPedal[address.channel];
    Voice* voice = impl_->findRestrike(address);
    if (voice != nullptr && voice->address.noteId >= 0 && voice->address.noteId != address.noteId) {
        impl_->endNote(voice->address); // the restruck string carries the new note from here
    }
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
    }
    impl_->updateUndamped();
}

void PianoEngine::choke(const NoteAddress& address) {
    for (auto& voice : impl_->voices) {
        if (Impl::matches(voice, address)) {
            voice.choking = true;
            voice.keyDown = false;
        }
    }
    impl_->updateUndamped();
}

void PianoEngine::setSustain(std::int16_t channel, double depth) {
    if (channel < 0 || channel > 15 || !std::isfinite(depth)) {
        return;
    }
    // The dampers follow as the pedal moves (movePedals).
    impl_->pedalTarget[channel] = std::clamp(depth, 0.0, 1.0);
}

void PianoEngine::setSoftPedal(std::int16_t channel, double depth) {
    if (channel >= 0 && channel <= 15 && std::isfinite(depth)) {
        impl_->softPedal[channel] = std::clamp(depth, 0.0, 1.0);
    }
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
        case ParameterId::pedalNoise:
            value = std::clamp(value, 0.0, 2.0);
            break;
        case ParameterId::sustainLength:
        case ParameterId::damperRelease:
            value = std::clamp(value, 0.25, 4.0);
            break;
        case ParameterId::unisonDetune:
            value = std::clamp(value, 0.0, 3.0);
            break;
        case ParameterId::masterTune:
            value = std::clamp(value, 415.0, 466.0);
            break;
        case ParameterId::stretch:
            value = std::clamp(value, 0.0, 2.0);
            break;
        case ParameterId::age:
            value = std::clamp(value, 0.0, 1.0);
            break;
    }
    const bool voicing = id == ParameterId::sustainLength || id == ParameterId::damperRelease ||
        id == ParameterId::unisonDetune || id == ParameterId::aftersound ||
        id == ParameterId::masterTune || id == ParameterId::stretch || id == ParameterId::age;
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
        impl_->currentFrame = frame;
        std::array<double, 3> regionLeft{};
        std::array<double, 3> regionRight{};
        std::array<double, 3> regionCentre{};
        std::array<double, 3> regionMono{};
        std::array<double, 3> regionSympathetic{};
        std::array<double, 3> regionKnock{};
        bool sounding = false;
        for (auto& voice : impl_->voices) {
            if (!voice.active || voice.key == nullptr) {
                continue;
            }
            sounding = true;
            const std::size_t region = voice.address.key < 48 ? 0 : (voice.address.key < 72 ? 1 : 2);
            const auto sample = impl_->processVoice(voice);
            const double sampleLeft = sample.left * voice.key->leftGain;
            const double sampleRight = sample.right * voice.key->rightGain;
            regionLeft[region] += sampleLeft;
            regionRight[region] += sampleRight;
            regionCentre[region] += sample.centre * 0.5 * (voice.key->leftGain + voice.key->rightGain);
            // Sympathetic strings hear the bridge, not the per-channel radiation phase.
            const double bridgeSample = 0.5 * sample.bridge * (voice.key->leftGain + voice.key->rightGain);
            regionMono[region] += bridgeSample;
            regionSympathetic[region] += bridgeSample * voice.key->sympatheticDrive;
            regionKnock[region] += sample.knock;
        }
        // The trapwork and damper rail span the case: their noise reaches every region alike.
        const auto [pedalKnock, pedalBrush] = impl_->movePedals();
        for (auto& drive : regionSympathetic) {
            drive += pedalBrush;
        }
        const auto resonance = impl_->sympathetic.process(regionSympathetic, impl_->undamped, sympatheticLevel);
        std::array<double, 3> bridge{};
        for (std::size_t region = 0; region < 3; ++region) {
            regionLeft[region] += resonance[region] * 0.707;
            regionRight[region] += resonance[region] * 0.707;
            regionCentre[region] += resonance[region] * 0.707;
            bridge[region] = regionMono[region] + resonance[region] + regionKnock[region] + pedalKnock;
        }
        double mixedLeft = 0.0;
        double mixedRight = 0.0;
        double mixedCentre = 0.0;
        impl_->soundboard.process(regionLeft, regionRight, regionCentre, bridge, bodyLevel, mixedLeft, mixedRight,
                                  mixedCentre);
        // Gain and width glide while notes sound and settle at once between them, so a bounce whose
        // host sends them at the start doesn't open with a glide. Per sample, so block size can't matter.
        impl_->smoothedGain += sounding ? 0.0002 * (targetGain - impl_->smoothedGain) : targetGain - impl_->smoothedGain;
        impl_->smoothedWidth += sounding ? 0.0002 * (targetWidth - impl_->smoothedWidth) : targetWidth - impl_->smoothedWidth;
        // Below 1, Stereo Width crossfades from one coherent microphone (0: a true mono that sums
        // without comb filtering at every Mic Position) to the pair; above 1 it widens mid/side.
        const double width = impl_->smoothedWidth;
        double wideLeft = 0.0;
        double wideRight = 0.0;
        if (width < 1.0) {
            wideLeft = mixedCentre + width * (mixedLeft - mixedCentre);
            wideRight = mixedCentre + width * (mixedRight - mixedCentre);
        } else {
            const double mid = 0.5 * (mixedLeft + mixedRight);
            const double side = 0.5 * (mixedLeft - mixedRight) * width;
            wideLeft = mid + side;
            wideRight = mid - side;
        }
        const double outputLeft = impl_->highpass(wideLeft, 0) * impl_->smoothedGain;
        const double outputRight = impl_->highpass(wideRight, 1) * impl_->smoothedGain;
        left[frame] = std::abs(outputLeft) < 1.0e-20 ? 0.0F : static_cast<float>(outputLeft);
        right[frame] = std::abs(outputRight) < 1.0e-20 ? 0.0F : static_cast<float>(outputRight);
    }
    impl_->currentFrame = 0;
}

void PianoEngine::modulate(const NoteAddress& target, ParameterId id, double amount) {
    if (!std::isfinite(amount) || (id != ParameterId::hammerHardness && id != ParameterId::hammerNoise)) {
        return;
    }
    const bool global = target.noteId < 0 && target.port < 0 && target.channel < 0 && target.key < 0;
    if (global) {
        impl_->modulation[parameterIndex(id)] = amount;
    }
    for (auto& voice : impl_->voices) {
        if (!Impl::targets(voice, target)) {
            continue;
        }
        if (!global) {
            (id == ParameterId::hammerHardness ? voice.hardnessModulation : voice.noiseModulation) = amount;
        }
        if (id == ParameterId::hammerHardness && voice.age == 0) {
            impl_->shape(voice); // sent with its note: the strike takes it
        }
    }
}

// An expression sent with its note-on (before any sample) applies at once, not as a glide.
void PianoEngine::setNoteVolume(const NoteAddress& target, double gain) {
    if (!std::isfinite(gain)) {
        return;
    }
    for (auto& voice : impl_->voices) {
        if (Impl::targets(voice, target)) {
            voice.volumeTarget = std::clamp(gain, 0.0, 4.0);
            if (voice.age == 0) {
                voice.volume = voice.volumeTarget;
            }
        }
    }
}

// Equal-power and unity at the centre, on top of the key's own placement.
void PianoEngine::setNotePan(const NoteAddress& target, double pan) {
    if (!std::isfinite(pan)) {
        return;
    }
    const double angle = std::clamp(pan, 0.0, 1.0) * 1.5707963267948966;
    for (auto& voice : impl_->voices) {
        if (Impl::targets(voice, target)) {
            voice.panLeftTarget = std::sqrt(2.0) * std::cos(angle);
            voice.panRightTarget = std::sqrt(2.0) * std::sin(angle);
            if (voice.age == 0) {
                voice.panLeft = voice.panLeftTarget;
                voice.panRight = voice.panRightTarget;
            }
        }
    }
}

void PianoEngine::setNoteBrightness(const NoteAddress& target, double brightness) {
    if (!std::isfinite(brightness)) {
        return;
    }
    for (auto& voice : impl_->voices) {
        if (Impl::targets(voice, target)) {
            voice.brightness = 2.0 * (std::clamp(brightness, 0.0, 1.0) - 0.5);
            if (voice.age == 0) {
                impl_->shape(voice); // sent with its note: the strike takes it
            }
        }
    }
}

std::size_t PianoEngine::takeEndedNotes(EndedNote* destination, std::size_t capacity) {
    auto& ended = impl_->endedNotes;
    const std::size_t count = std::min(capacity, impl_->endedNoteCount);
    std::copy_n(ended.begin(), count, destination);
    std::copy(ended.begin() + count, ended.begin() + impl_->endedNoteCount, ended.begin());
    impl_->endedNoteCount -= count;
    return count;
}

std::size_t PianoEngine::activeVoiceCount() const {
    return static_cast<std::size_t>(std::count_if(impl_->voices.begin(), impl_->voices.end(),
        [](const Voice& voice) { return voice.active; }));
}

} // namespace pianocraft
