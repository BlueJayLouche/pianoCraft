#include "engine/StruckEngine.hpp"

#include <algorithm>
#include <cmath>

namespace pianocraft {

namespace {

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double ln1000 = 6.907755278982138; // T60: a decay of 60 dB

std::size_t index(StruckParameterId id) {
    return static_cast<std::size_t>(id);
}

// Crotales C6-C8, fitted to the University of Iowa recordings (25 discs, pp and ff; tools/iowa_calibration.py
// modes crotales): the fundamental rings longest, the modes above it are strongest at the strike, more so in the
// lower discs, and die within a second. Their ratios close in up the range. From ff to pp a mode falls
// 8 + 16 log2(f / 1 kHz) dB: steeper than the modes' own fit (20.5 + 7.43), which leaves pp 4 dB too bright, and
// matching the whole strokes, ff 37 dB louder and 5 dB brighter (docs/celesta-and-crotales.md).
constexpr StruckModel crotales{
    84, 108, 84,
    {{{1.0, 0.0, 0.0, 0.0, 2.565, -0.794, 0.0},
      {2.136, -0.115, 11.8, -2.88, 2.375, -1.382, 0.0},
      {3.740, -0.275, 27.6, -15.6, 1.145, -1.156, 0.0},
      {5.717, -0.501, 28.5, -25.7, -0.502, -0.131, 0.0},
      {8.027, -0.851, 27.0, -33.5, -0.806, 0.248, 0.0},
      {10.504, -0.947, 18.8, -42.2, -0.525, -0.299, 0.0},
      {13.464, -1.887, 7.5, -24.3, -0.656, -0.156, 0.0}}},
    16.0, 8.0, 16.0, false, {0.0, 2.0}, {6.0, 1.2}, 9.0, 0, 0.3, 0.0, 0.003};

// The celesta, C4-C9: steel bars, their modes and decay measured on the Iowa orchestra bells (steel bars,
// plastic mallets, C5-E8; tools/iowa_calibration.py modes bells). By judgment, as there is no celesta corpus: the
// resonator under each bar takes the fundamental's energy faster (0.4 of the bare bar's T60), and the felt hammer
// strikes the upper modes 20 dB softer than the bells' plastic mallets. Each key has the bells' 26 dB from pp to ff,
// and its upper modes the crotales' softening with frequency.
constexpr StruckModel celesta{
    60, 120, 84,
    {{{1.0, 0.0, 0.0, 0.0, 1.692, 0.127, -0.491},
      {3.11, 0.0, -36.0, 0.0, 0.0, 0.0, 0.0},
      {6.99, 0.0, -27.0, 0.0, -0.357, 0.0, 0.0},
      {10.46, 0.0, -40.0, 0.0, 0.182, 0.0, 0.0},
      {15.06, 0.0, -46.0, 0.0, 0.0, 0.0, 0.0},
      {},
      {}}},
    0.0, 26.0, 16.0, true, {-1.0, 2.333}, {8.0, 1.6}, 0.0, 0, 0.2, 1.0, 0.55};

// The vibraphone, C3-F6: aluminium bars tuned 1:4:10 over resonators, fitted to the Iowa vibraphone
// (pedal down, motor off; tools/iowa_calibration.py modes vibraphone): the tuned 4th and 10th, a torsional mode
// falling through them and the 4th bending mode, strong only in the low bars. From pp to ff, 30 dB at the
// fundamental by judgment (the recordings: mf 8 dB under ff, pp 46 dB), the upper modes softening 8.4 dB an
// octave above it (fitted at mf). The motor and its depth are by judgment: the recordings have none.
constexpr StruckModel vibraphone{
    48, 89, 84,
    {{{1.0, 0.0, 0.0, 0.0, 2.451, -0.176, -0.006},
      {3.932, -0.034, -29.1, -2.7, -0.149, -0.683, 0.0},
      {9.791, -0.084, -48.0, -18.3, -1.392, -0.432, 0.0},
      {2.035, -2.644, -61.0, -11.7, 1.947, 0.219, 0.0},
      {10.163, -1.973, -100.6, -32.7, -2.626, -1.347, 0.0},
      {},
      {}}},
    2.8, 30.0, 8.4, true, {-3.0, 0.417}, {10.0, 0.3}, 0.0, 0, 0.3, 1.0, 0.37};

// The handpan, C3-C6 and chromatic (a real one has eight or nine of these notes): a hammered steel tone field per
// key, tuned so its first three modes stand at 1:2:3, struck by the fingers. By judgment, after Rossing et al. on
// the Hang: the octave a little under the fundamental, the twelfth well under, the fundamental ringing longest. The
// octave and twelfth each beat gently against a near twin; the fundamental is clean.
constexpr StruckModel handpan{
    48, 84, 62,
    {{{1.0, 0.0, 0.0, 0.0, 1.6, -0.35, 0.0},
      {2.0, 0.0, -6.0, 0.0, 1.1, -0.35, 0.0},
      {3.0, 0.0, -14.0, 0.0, 0.4, -0.35, 0.0},
      {},
      {},
      {},
      {}}},
    0.0, 24.0, 10.0, true, {-1.167, 1.833}, {3.0, 0.2}, 1.5, 1, 0.15, 0.0, 0.35};

// The same bar or disc every time: a uniform number in [-1, 1) from the key and a slot.
double scatter(int key, int slot) {
    std::uint64_t z = static_cast<std::uint64_t>(key) * 0x9E3779B97F4A7C15ull + static_cast<std::uint64_t>(slot) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<double>(z >> 11) / 4503599627370496.0 - 1.0;
}

} // namespace

StruckEngine::StruckEngine(const StruckModel& model) : model_(model) {
    parameters_[index(StruckParameterId::outputGain)] = 0.7;
    parameters_[index(StruckParameterId::brightness)] = 1.0;
    parameters_[index(StruckParameterId::damping)] = model.damping;
    parameters_[index(StruckParameterId::sustainLength)] = 1.0;
    parameters_[index(StruckParameterId::stereoWidth)] = 1.0;
    parameters_[index(StruckParameterId::masterTune)] = 440.0;
    parameters_[index(StruckParameterId::motorDepth)] = 0.7;
}

CelestaEngine::CelestaEngine() : StruckEngine(celesta) {}

CrotalesEngine::CrotalesEngine() : StruckEngine(crotales) {}

VibraphoneEngine::VibraphoneEngine() : StruckEngine(vibraphone) {}

HandpanEngine::HandpanEngine() : StruckEngine(handpan) {}

void StruckEngine::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    reset();
}

void StruckEngine::reset() {
    for (auto& voice : voices_) {
        voice = Voice{};
    }
    bend_ = {};
    sustain_ = {};
    expression_ = expressionTarget_ = 1.0;
    motorPhase_ = 0.0;
    motorRate_ = parameters_[index(StruckParameterId::motorSpeed)];
    motorDepth_ = motorRate_ > 0.0 ? parameters_[index(StruckParameterId::motorDepth)] : 0.0;
    endedCount_ = 0;
}

double StruckEngine::tuningFactor(std::int16_t channel) const {
    const double bend = channel >= 0 && channel < 16 ? bend_[static_cast<std::size_t>(channel)] : 0.0;
    return parameters_[index(StruckParameterId::masterTune)] / 440.0 * std::pow(2.0, 2.0 * bend / 12.0);
}

void StruckEngine::retune(Voice& voice) {
    const double factor = tuningFactor(voice.address.channel);
    for (std::size_t i = 0; i < voice.modeCount; ++i) {
        auto& mode = voice.modes[i];
        const double omega = 2.0 * pi * mode.frequency * factor / sampleRate_;
        mode.a1 = omega < 0.95 * pi ? 2.0 * mode.radius * std::cos(omega) : 0.0;
        mode.a2 = omega < 0.95 * pi ? mode.radius * mode.radius : 0.0;
    }
}

void StruckEngine::endNote(const NoteAddress& address) {
    if (endedCount_ < ended_.size()) {
        ended_[endedCount_++] = {address, frame_};
    }
}

// The mallet sets each mode ringing from rest (sine phase). Velocity sets the level: a mode at f is
// dynamicRange + softening log2(f / 1 kHz) dB quieter at pp than at ff, the softer stroke's longer contact
// passing less of the high modes. Brightness moves the softening as velocity does, leaving the fundamental's
// level. A ringing bar struck again adds to its motion: the modes are linear.
void StruckEngine::strike(Voice& voice, double velocity) {
    const auto& p = parameters_;
    const int key = voice.address.key;
    const double x = (key - model_.referenceKey) / 12.0;
    const double reach = std::clamp(x, model_.reach[0], model_.reach[1]);
    const double soft = std::log(std::max(velocity, 0.02) / 0.9) / std::log(0.35 / 0.9); // 0 at ff, 1 at pp
    const double brightness = std::clamp(p[index(StruckParameterId::brightness)] + globalModulation_[index(StruckParameterId::brightness)] +
                                             voice.brightnessModulation + voice.timbre, 0.25, 3.0);
    const double darker = std::clamp(soft - (brightness - 1.0), -1.0, 3.0);
    const double fundamental = 440.0 * std::pow(2.0, (key - 69) / 12.0);
    const double base = model_.fundamentalSlope * x -
                        soft * (model_.dynamicRange + (model_.keyed ? 0.0 : model_.softening * std::log2(fundamental / 1000.0)));
    const double sustain = std::max(p[index(StruckParameterId::sustainLength)], 0.05);
    const double limit = 0.45 * sampleRate_ / std::max(tuningFactor(voice.address.channel), 0.5);

    std::array<Mode, modeLimit> modes{};
    std::array<double, modeLimit> amplitude{};
    std::size_t count = 0;
    for (std::size_t n = 0; n < model_.modes.size() && count < modeLimit; ++n) {
        const auto& law = model_.modes[n];
        if (law.r0 <= 0.0) {
            break;
        }
        const double u = n == 0 ? 0.0 : scatter(key, static_cast<int>(n));
        const double frequency = fundamental * (law.r0 + law.r1 * reach) * (1.0 + 0.01 * model_.scatter[1] * scatter(key, 10 + static_cast<int>(n)) * (n > 0));
        if (frequency > limit) {
            break;
        }
        const double t60 = std::clamp(sustain * std::exp(law.t0 + law.t1 * reach + law.t2 * reach * reach), 0.05, 60.0);
        const double radius = std::exp(-ln1000 / (t60 * sampleRate_));
        const double db = base + law.l0 + law.l1 * x + model_.scatter[0] * u - darker * model_.softening * std::log2(frequency / fundamental);
        modes[count] = {frequency, radius};
        amplitude[count++] = std::pow(10.0, db / 20.0);
        // A disc's modes come in near-degenerate pairs, split by its imperfections: they beat.
        if (model_.beating > 0.0 && static_cast<int>(n) >= model_.beatingFrom && n < 3 && count < modeLimit) {
            const double split = 0.4 * std::pow(model_.beating / 0.4, 0.5 + 0.5 * scatter(key, 20 + static_cast<int>(n)));
            const double partner = -14.0 + 11.0 * scatter(key, 30 + static_cast<int>(n));
            modes[count] = {frequency + (scatter(key, 40 + static_cast<int>(n)) < 0.0 ? -split : split), radius};
            amplitude[count] = amplitude[count - 1] * std::pow(10.0, partner / 20.0);
            ++count;
        }
    }
    // Struck again while ringing: same modes, so keep their motion.
    const bool ringing = voice.modeCount == count;
    for (std::size_t i = 0; i < count; ++i) {
        if (ringing) {
            modes[i].y1 = voice.modes[i].y1;
            modes[i].y2 = voice.modes[i].y2;
        }
    }
    voice.modes = modes;
    voice.modeCount = count;
    voice.damped = false;
    retune(voice);
    const double factor = tuningFactor(voice.address.channel);
    for (std::size_t i = 0; i < count; ++i) {
        auto& mode = voice.modes[i];
        const double omega = 2.0 * pi * mode.frequency * factor / sampleRate_;
        mode.y2 -= amplitude[i] * std::sin(omega) / mode.radius; // the next sample is A r sin(omega)
    }
    const auto channel = static_cast<std::size_t>(std::clamp<int>(voice.address.channel, 0, 15));
    if (!voice.keyDown && !sustain_[channel]) {
        damp(voice); // released before it was struck
    }
}

void StruckEngine::damp(Voice& voice) {
    const double damping = parameters_[index(StruckParameterId::damping)];
    if (damping <= 0.0 || voice.damped) {
        return;
    }
    // The damper, or the player's hand: at 1 every mode dies in about 0.12 s.
    for (std::size_t i = 0; i < voice.modeCount; ++i) {
        auto& mode = voice.modes[i];
        const double rate = -std::log(mode.radius) * sampleRate_ / ln1000 + damping / 0.12;
        mode.radius = std::exp(-ln1000 * rate / sampleRate_);
    }
    voice.damped = true;
    retune(voice);
}

void StruckEngine::noteOn(const NoteAddress& address, double velocity) {
    if (address.key < model_.lowestKey || address.key > model_.highestKey) {
        endNote(address);
        return;
    }
    if (activeVoiceCount() == 0) {
        motorPhase_ = 0.0; // unheard while all is still: a passage from silence renders the same every time
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
        // Every voice ringing: give up the quietest released one, else the quietest.
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
    voice->brightnessModulation = voice->timbre = 0.0;
    voice->pendingVelocity = std::clamp(velocity, 0.0, 1.0);
}

void StruckEngine::noteOff(const NoteAddress& address, double) {
    for (auto& voice : voices_) {
        if (voice.active && voice.keyDown && matches(address, voice.address)) {
            voice.keyDown = false;
            const auto channel = static_cast<std::size_t>(std::clamp<int>(voice.address.channel, 0, 15));
            if (!sustain_[channel] && voice.pendingVelocity < 0.0) {
                damp(voice);
            }
        }
    }
}

void StruckEngine::choke(const NoteAddress& address) {
    for (auto& voice : voices_) {
        if (voice.active && matches(address, voice.address)) {
            endNote(voice.address);
            voice = Voice{};
        }
    }
}

void StruckEngine::controlChange(std::int16_t channel, std::uint8_t controller, double value) {
    if (controller == 11) {
        expressionTarget_ = std::clamp(value, 0.0, 1.0);
    } else if (controller == 64 && channel >= 0 && channel < 16) {
        sustain_[static_cast<std::size_t>(channel)] = value >= 0.5;
        if (value < 0.5) {
            for (auto& voice : voices_) {
                if (voice.active && !voice.keyDown && voice.address.channel == channel && voice.pendingVelocity < 0.0) {
                    damp(voice);
                }
            }
        }
    }
}

void StruckEngine::pitchBend(std::int16_t channel, double bend) {
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

void StruckEngine::setParameter(StruckParameterId id, double value) {
    const auto i = index(id);
    if (i == 0 || i >= parameterCount || !std::isfinite(value)) {
        return;
    }
    parameters_[i] = value;
    if (id == StruckParameterId::masterTune) {
        for (auto& voice : voices_) {
            if (voice.active && voice.pendingVelocity < 0.0) {
                retune(voice);
            }
        }
    }
}

double StruckEngine::parameterValue(StruckParameterId id) const {
    const auto i = index(id);
    return i < parameterCount ? parameters_[i] : 0.0;
}

void StruckEngine::modulate(const NoteAddress& target, StruckParameterId id, double amount) {
    if (!std::isfinite(amount) || id != StruckParameterId::brightness) {
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

void StruckEngine::setNoteVolume(const NoteAddress& target, double gain) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(gain)) {
            voice.volumeTarget = std::clamp(gain, 0.0, 4.0);
        }
    }
}

void StruckEngine::setNotePan(const NoteAddress& target, double pan) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(pan)) {
            voice.pan = std::clamp(pan, 0.0, 1.0);
        }
    }
}

void StruckEngine::setNoteBrightness(const NoteAddress& target, double brightness) {
    for (auto& voice : voices_) {
        if (voice.active && matches(target, voice.address) && std::isfinite(brightness)) {
            voice.timbre = 2.0 * (std::clamp(brightness, 0.0, 1.0) - 0.5);
        }
    }
}

std::size_t StruckEngine::takeEndedNotes(EndedNote* destination, std::size_t capacity) {
    const std::size_t count = std::min(capacity, endedCount_);
    std::copy_n(ended_.begin(), count, destination);
    endedCount_ = 0;
    return count;
}

std::size_t StruckEngine::activeVoiceCount() const {
    return static_cast<std::size_t>(std::count_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.active; }));
}

void StruckEngine::render(float* left, float* right, std::uint32_t frameCount) {
    const double width = std::clamp(parameters_[index(StruckParameterId::stereoWidth)], 0.0, 1.0);
    const double output = parameters_[index(StruckParameterId::outputGain)] * model_.level;
    const double middle = 0.5 * (model_.lowestKey + model_.highestKey);
    const double half = 0.5 * (model_.highestKey - model_.lowestKey);
    // Low to the left, high to the right, as the player sits; amplitude panning only, so a mono sum
    // can't comb-filter.
    std::array<double, voiceCapacity> gainLeft{};
    std::array<double, voiceCapacity> gainRight{};
    for (std::size_t v = 0; v < voiceCapacity; ++v) {
        auto& voice = voices_[v];
        if (!voice.active) {
            continue;
        }
        if (voice.pendingVelocity >= 0.0) {
            strike(voice, voice.pendingVelocity);
            voice.pendingVelocity = -1.0;
        }
        const double spread = std::clamp((voice.address.key - middle) / half, -1.0, 1.0) * model_.spread * width;
        const double place = std::clamp(0.5 + spread + (voice.pan - 0.5), 0.0, 1.0);
        // cos on both sides, so the centre is equal to the bit (Stereo Width 0 is exact mono).
        gainLeft[v] = std::cos(place * 0.5 * pi);
        gainRight[v] = std::cos((1.0 - place) * 0.5 * pi);
    }
    const double glide = 100.0 / sampleRate_;
    const double release = 0.05 * sampleRate_;
    // The vibraphone's motor turns a disc in the top of each resonator, which reinforces only the bar's
    // fundamental (and its beating partner): the upper modes ring on unmodulated. Stopped, the discs stand open.
    const double speed = std::clamp(parameters_[index(StruckParameterId::motorSpeed)], 0.0, 12.0);
    const double depth = speed > 0.0 ? std::clamp(parameters_[index(StruckParameterId::motorDepth)], 0.0, 1.0) : 0.0;
    const std::size_t resonated = model_.beating > 0.0 && model_.beatingFrom == 0 ? 2 : 1;
    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        frame_ = frame;
        motorRate_ += glide * (speed - motorRate_);
        motorDepth_ += glide * (depth - motorDepth_);
        motorPhase_ = std::fmod(motorPhase_ + 2.0 * pi * motorRate_ / sampleRate_, 2.0 * pi);
        const double motor = 1.0 - motorDepth_ * 0.5 * (1.0 - std::cos(motorPhase_));
        double sumLeft = 0.0;
        double sumRight = 0.0;
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
                sample += i < resonated ? motor * mode.y1 : mode.y1;
            }
            voice.volume += glide * (voice.volumeTarget - voice.volume);
            sample *= voice.volume;
            voice.level = std::max(std::abs(sample), voice.level * 0.9999);
            sumLeft += sample * gainLeft[v];
            sumRight += sample * gainRight[v];
            if (++voice.age > release && voice.level < 1.0e-6) {
                endNote(voice.address);
                voice = Voice{};
            }
        }
        expression_ += glide * (expressionTarget_ - expression_);
        const double gain = output * expression_;
        left[frame] = static_cast<float>(gain * sumLeft);
        right[frame] = static_cast<float>(gain * sumRight);
    }
    frame_ = 0;
}

} // namespace pianocraft
