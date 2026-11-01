#include "engine/FluteEngine.hpp"
#include "engine/Filters.hpp"

#include <algorithm>
#include <cmath>

namespace pianocraft {

namespace {

using namespace filters;
constexpr int lowestKey = 59;  // B3, with the B foot
constexpr int highestKey = 98; // D7: below it C3 hardly spoke, and nothing above is a flute's

// Loop constants (analytic, set by ear and by the engine tests' pitch and stability checks).
constexpr double jetFeedback = 2.2;  // bore wave deflecting the jet, per unit jet speed
constexpr double jetOffset = 1.0;    // the jet aims a little off the labium: even harmonics
constexpr double jetDrive = 0.6;     // flow into the bore per unit jet speed
constexpr double endReflection = 0.7;
// The jet's flight, as a share of the period. At 0.5 the twelfth is in phase too and the low
// register overblows; 0.4 keeps the third harmonic 7-12 dB down and plays a constant 52 cents
// sharp, which jetTuning takes back.
constexpr double jetRatio = 0.4;
const double jetTuning = std::pow(2.0, 52.0 / 1200.0);
// Breath makes the tone, as Fletcher measured (JASA 57, 1975): the upper partials grow with it,
// while the fundamental moves little in the low octave and considerably in the third. The loop's own
// jet saturates at every breath that speaks, so the radiated partials come from a second jet split
// at the labium, driven by the bore wave at unit amplitude: nearly linear at labiumSoft, rich at
// labiumLoud. Its fundamental is normalised out, so the loop alone sets the note's level.
constexpr double labiumSoft = 0.3;
constexpr double labiumLoud = 5.0;
constexpr double labiumOffset = 0.9; // the even partials
constexpr double labiumStep = 0.1;   // drive step of labiumFundamental's table

// The fundamental's amplitude of tanh(drive cos t + labiumOffset) - tanh(labiumOffset), for drive
// 0, labiumStep, ... 6.4.
double labiumFundamental(double drive) {
    static const auto table = [] {
        std::array<double, 65> t{};
        for (std::size_t i = 0; i < t.size(); ++i) {
            for (int m = 0; m < 64; ++m) {
                const double c = std::cos(2.0 * pi * m / 64.0);
                t[i] += (std::tanh(labiumStep * static_cast<double>(i) * c + labiumOffset) - std::tanh(labiumOffset)) * c / 32.0;
            }
        }
        return t;
    }();
    const double x = std::clamp(drive / labiumStep, 1.0, static_cast<double>(table.size() - 1) - 1e-9);
    const auto i = static_cast<std::size_t>(x);
    return table[i] + (x - static_cast<double>(i)) * (table[i + 1] - table[i]);
}

std::size_t index(FluteParameterId id) {
    return static_cast<std::size_t>(id);
}

} // namespace

FluteEngine::FluteEngine() {
    parameters_[index(FluteParameterId::outputGain)] = 0.7;
    parameters_[index(FluteParameterId::breathNoise)] = 1.0;
    parameters_[index(FluteParameterId::vibratoDepth)] = 1.0;
    parameters_[index(FluteParameterId::vibratoRate)] = 5.0;
    parameters_[index(FluteParameterId::brightness)] = 1.0;
    parameters_[index(FluteParameterId::legatoTime)] = 0.012;
    parameters_[index(FluteParameterId::masterTune)] = 440.0;
}

void FluteEngine::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    reset();
}

void FluteEngine::reset() {
    sounding_ = keyDown_ = false;
    releasedAt_ = -1.0;
    slurSeconds_ = 1.0e9;
    envelope_ = level_ = jetLow_ = 0.0;
    bore_ = {};
    jet_ = {};
    write_ = 0;
    reflection_ = dcInput_ = dcOutput_ = radiated_ = noiseLow_ = airLow_ = airHigh_ = 0.0;
    formant_ = {};
    boreLevel_ = 0.0;
    noise_ = 0x9E3779B9u;
    breath_ = breathTarget_ = 0.8;
    expression_ = expressionTarget_ = 1.0;
    vibratoAmount_ = 1.0;
    vibratoPhase_ = 0.0;
    bend_ = 0.0;
    endedCount_ = 0;
}

double FluteEngine::period(int key) const {
    return sampleRate_ / (parameters_[index(FluteParameterId::masterTune)] * std::pow(2.0, (key - 69) / 12.0));
}

void FluteEngine::endNote(const NoteAddress& address) {
    if (endedCount_ < ended_.size()) {
        ended_[endedCount_++] = {address, frame_};
    }
}

void FluteEngine::startNote(const NoteAddress& address, double velocity, bool legato) {
    if (sounding_) {
        endNote(note_); // the slur or re-attack takes over the sounding note's voice
    }
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
        attackSeconds_ = 0.09 - 0.06 * velocity;
        noteSeconds_ = 0.0;
        period_ = periodTarget_;
        envelope_ = 0.0; // the tongue stops the air before it releases it again
    } else {
        slurSeconds_ = 0.0;
    }
}

void FluteEngine::noteOn(const NoteAddress& address, double velocity) {
    if (address.key < lowestKey || address.key > highestKey) {
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
              (keyDown_ || joined) && speaking && parameters_[index(FluteParameterId::legatoTime)] >= 0.0);
}

void FluteEngine::noteOff(const NoteAddress& address, double) {
    if (keyDown_ && matches(address, note_)) {
        keyDown_ = false;
        releasedAt_ = noteSeconds_;
    }
}

void FluteEngine::choke(const NoteAddress& address) {
    if (sounding_ && matches(address, note_)) {
        endNote(note_);
        sounding_ = keyDown_ = false;
        envelope_ = 0.0;
        bore_ = {};
        jet_ = {};
    }
}

void FluteEngine::controlChange(std::int16_t, std::uint8_t controller, double value) {
    value = std::clamp(value, 0.0, 1.0);
    if (controller == 1) {
        breathTarget_ = value;
    } else if (controller == 11) {
        expressionTarget_ = value;
    } else if (controller == 21) {
        vibratoAmount_ = 2.0 * value;
    }
}

void FluteEngine::pitchBend(std::int16_t, double bend) {
    if (std::isfinite(bend)) {
        bend_ = std::clamp(bend, -1.0, 1.0);
    }
}

void FluteEngine::setParameter(FluteParameterId id, double value) {
    const auto i = index(id);
    if (i != 0 && i < parameterCount && std::isfinite(value)) {
        parameters_[i] = value;
        if (id == FluteParameterId::masterTune && sounding_) {
            periodTarget_ = period(note_.key);
        }
    }
}

double FluteEngine::parameterValue(FluteParameterId id) const {
    const auto i = index(id);
    return i < parameterCount ? parameters_[i] : 0.0;
}

void FluteEngine::modulate(const NoteAddress& target, FluteParameterId id, double amount) {
    const bool global = target.noteId < 0 && target.port < 0 && target.channel < 0 && target.key < 0;
    if (id == FluteParameterId::brightness && std::isfinite(amount) && (global || matches(target, note_))) {
        brightnessModulation_ = amount;
    }
}

void FluteEngine::setNoteVolume(const NoteAddress& target, double gain) {
    if (sounding_ && matches(target, note_) && std::isfinite(gain)) {
        volumeTarget_ = std::clamp(gain, 0.0, 4.0);
    }
}

void FluteEngine::setNotePan(const NoteAddress& target, double pan) {
    if (sounding_ && matches(target, note_) && std::isfinite(pan)) {
        pan_ = std::clamp(pan, 0.0, 1.0);
    }
}

void FluteEngine::setNoteBrightness(const NoteAddress& target, double brightness) {
    if (sounding_ && matches(target, note_) && std::isfinite(brightness)) {
        timbre_ = 2.0 * (std::clamp(brightness, 0.0, 1.0) - 0.5);
    }
}

void FluteEngine::setNotePressure(const NoteAddress& target, double pressure) {
    if (sounding_ && matches(target, note_) && std::isfinite(pressure)) {
        breathTarget_ = std::clamp(pressure, 0.0, 1.0);
    }
}

std::size_t FluteEngine::takeEndedNotes(EndedNote* destination, std::size_t capacity) {
    const std::size_t count = std::min(capacity, endedCount_);
    std::copy_n(ended_.begin(), count, destination);
    endedCount_ = 0;
    return count;
}

std::size_t FluteEngine::activeVoiceCount() const {
    return sounding_ ? 1 : 0;
}

void FluteEngine::render(float* left, float* right, std::uint32_t frameCount) {
    const auto& p = parameters_;
    if (!sounding_) {
        std::fill_n(left, frameCount, 0.0F);
        std::fill_n(right, frameCount, 0.0F);
        return;
    }
    const double fs = sampleRate_;
    const double rateScale = 48000.0 / fs;
    // The open end loses treble: a one-pole low-pass, darker at low Brightness.
    const double brightness = std::clamp(p[index(FluteParameterId::brightness)] + brightnessModulation_ + timbre_, 0.25, 3.0);
    const double pole = std::pow(std::clamp(0.45 - 0.15 * (brightness - 1.0), 0.1, 0.8), rateScale);
    const double dcPole = std::pow(0.9995, rateScale);
    const double smooth = 1.0 - std::exp(-1.0 / (0.015 * fs));
    const double glide = 1.0 - std::exp(-1.0 / (0.01 * fs));
    const double legato = 1.0 - std::exp(-3.0 / (std::max(p[index(FluteParameterId::legatoTime)], 0.001) * fs));
    const double attack = 1.0 - std::exp(-1.0 / (attackSeconds_ * fs));
    const double releaseRate = 1.0 - std::exp(-1.0 / (0.06 * fs));
    const double bendFactor = std::pow(2.0, 2.0 * bend_ / 12.0);
    const double vibratoStep = 2.0 * pi * std::clamp(p[index(FluteParameterId::vibratoRate)], 0.5, 12.0) / fs;
    const double vibratoDepth = std::max(p[index(FluteParameterId::vibratoDepth)], 0.0) * vibratoAmount_;
    const double noiseLevel = std::max(p[index(FluteParameterId::breathNoise)], 0.0);
    const double air = 1.0 - std::exp(-2.0 * pi * 5000.0 / fs);
    const double airFloor = 1.0 - std::exp(-2.0 * pi * 800.0 / fs);
    const double gainLeft = std::min(1.0, 2.0 * (1.0 - pan_));
    const double gainRight = std::min(1.0, 2.0 * pan_);
    const double output = 0.17 * p[index(FluteParameterId::outputGain)];
    const double highpass = 1.0 - std::exp(-0.5 * pi / period_);
    // Fletcher's formant: at forte it rises 12 dB/octave to 500 Hz and falls 12 dB/octave above
    // 1 kHz; at piano it falls from 500 Hz and barely rises. Normalised at f0, it shapes the
    // partials and leaves the fundamental's level to the jet.
    const double formantBreath = std::clamp(breath_, 0.0, 1.0);
    const auto rise = butterworth(true, std::max(500.0 * formantBreath, 40.0), fs);
    const double fallPole = std::exp(-2.0 * pi * 500.0 * std::pow(2.0, formantBreath) / fs);
    const std::array<double, 5> fall{1.0 - fallPole, 0.0, 0.0, -fallPole, 0.0};
    const double fundamental = 2.0 * pi * bendFactor / period_;
    const double formantGain = 1.0 / (magnitude(rise, fundamental) * magnitude(fall, fundamental));
    const double follow = 1.0 - std::exp(-1.0 / (0.03 * fs));
    // Players blow harder for higher notes (Fletcher: pressure ~ frequency), so the jet is faster and
    // its air louder: as jet speed, the square root of the pressure, from C4 up.
    const double airGain = std::sqrt(std::max(fs / period_ / 261.6, 1.0));
    // Above 1 kHz the labium's partials would alias, and Fletcher's third octave is nearly pure.
    const double labiumRegister = std::min(1.0, std::pow(1000.0 * period_ / (fs * bendFactor), 2.0));
    // How far up the flute: 0 at C4, 1 at C7. The loop loses more per period at high notes, so its
    // gain rises (or C7 and up don't speak softly), and it runs 5 cents sharp per unit, taken back.
    const double height = std::max((fs / period_ / 261.6 - 1.0) / 7.0, 0.0);
    const double jetRegister = 1.0 + 0.3 * height;
    const double registerTuning = std::pow(2.0, 5.0 * height / 1200.0);
    // The fundamental follows breath as breath^fundamentalLaw: 0.4 at C4, 1.2 from C7.
    const double fundamentalLaw = 0.4 + 0.8 * std::min(height, 1.0);

    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        frame_ = frame;
        breath_ += smooth * (breathTarget_ - breath_);
        expression_ += glide * (expressionTarget_ - expression_);
        volume_ += glide * (volumeTarget_ - volume_);
        if (keyDown_) {
            // The tongue releases a puff that settles to the held breath within ~50 ms.
            const double target = 1.0 + 0.35 * velocity_ * std::exp(-noteSeconds_ / 0.05);
            envelope_ += attack * (target - envelope_);
        } else {
            envelope_ -= releaseRate * envelope_;
        }
        noteSeconds_ += 1.0 / fs;
        slurSeconds_ += 1.0 / fs;
        // A slur is the keys moving, about 10 ms (Almeida, Chow, Smith and Wolfe, JASA 2009), not a
        // glide: the bore's length changes that fast, and while the new resonance takes over from
        // the old the jet is disturbed, a little less steady and a little noisier for ~15 ms.
        const double slur = std::exp(-slurSeconds_ / 0.012);
        period_ += legato * (periodTarget_ - period_);

        // Players let vibrato in after the note has spoken.
        const double vibrato = std::sin(vibratoPhase_) * vibratoDepth * std::clamp((noteSeconds_ - 0.25) / 0.6, 0.0, 1.0);
        vibratoPhase_ = std::fmod(vibratoPhase_ + vibratoStep, 2.0 * pi);
        // CC1 sets the jet's speed; the pressure behind it is its square. Vibrato pulses that
        // pressure by +-10% (Fletcher), so it is mostly a vibrato of the upper partials.
        const double breath = std::max(breath_ * (1.0 + 0.05 * vibrato), 0.0);
        const double jetSpeed = envelope_ * std::pow(breath, fundamentalLaw) * (1.0 - 0.15 * slur);
        const double cycle = period_ * jetTuning * registerTuning / bendFactor * (1.0 - 0.0025 * vibrato);

        // Delays: the bore round trip less its filters' phase delay at the fundamental, and the
        // jet's flight over half a period less its own low-pass (corner 2 f0: the jet can't follow
        // the higher harmonics as well), which keeps it in the first register.
        const double omega = 2.0 * pi / cycle;
        const double loopDelay = phaseDelay(1.0 - pole, 0.0, pole, omega) + phaseDelay(1.0, -1.0, dcPole, omega);
        const double boreDelay = std::clamp(cycle - loopDelay, 4.0, static_cast<double>(delaySize - 4));
        const double jetPole = std::exp(-2.0 * omega);
        const double jetDelay = std::clamp(jetRatio * cycle - phaseDelay(1.0 - jetPole, 0.0, jetPole, omega), 1.0,
                                           static_cast<double>(delaySize - 2));

        // Cubic Lagrange read of the bore.
        const double position = static_cast<double>(write_ + delaySize) - boreDelay;
        const auto base = static_cast<std::size_t>(position);
        const double d = position - static_cast<double>(base);
        const auto at = [&](std::size_t offset) { return bore_[(base + offset + delaySize - 1) % delaySize]; };
        const double ym1 = at(0), y0 = at(1), y1 = at(2), y2 = at(3);
        const double boreOut = y0 + d * (0.5 * (y1 - ym1) + d * (ym1 - 2.5 * y0 + 2.0 * y1 - 0.5 * y2 +
                                                                  d * (1.5 * (y0 - y1) + 0.5 * (y2 - ym1))));
        reflection_ = (1.0 - pole) * boreOut + pole * reflection_;

        noise_ ^= noise_ << 13;
        noise_ ^= noise_ >> 17;
        noise_ ^= noise_ << 5;
        const double white = static_cast<double>(noise_) / 2147483648.0 - 1.0;
        noiseLow_ += 0.3 * (white - noiseLow_);

        // The jet: deflected by the returning wave (plus its own turbulence), it reaches the
        // labium a half period later and splits there as tanh (Fletcher; Verge).
        jetLow_ += (1.0 - jetPole) * (-jetFeedback * jetRegister * (0.8 + 0.4 * breath) * reflection_ / (jetSpeed + 0.02) - jetLow_);
        // The tongue's release is turbulent for its first ~20 ms, whatever Breath Noise says: it is
        // what starts the note.
        jet_[write_] = jetLow_ + 0.05 * noiseLow_ * (noiseLevel + 8.0 * std::exp(-noteSeconds_ / 0.02) + 4.0 * slur);
        const double jetPosition = static_cast<double>(write_ + delaySize) - jetDelay;
        const auto jetBase = static_cast<std::size_t>(jetPosition);
        const double jetFraction = jetPosition - static_cast<double>(jetBase);
        const double deflection = jet_[jetBase % delaySize] * (1.0 - jetFraction) + jet_[(jetBase + 1) % delaySize] * jetFraction;
        const double flow = jetSpeed * (std::tanh(deflection + jetOffset) - std::tanh(jetOffset));

        const double input = jetDrive * flow + endReflection * reflection_;
        dcOutput_ = input - dcInput_ + dcPole * dcOutput_;
        dcInput_ = input;
        bore_[write_] = std::abs(dcOutput_) < 1.0e-30 ? 0.0 : dcOutput_;
        write_ = (write_ + 1) % delaySize;

        // The bore's pressure, high-passed for safety at a quarter of the fundamental, split again at
        // the labium and shaped by the formant. The breath's own air is band-limited noise riding
        // on the jet.
        radiated_ += highpass * (boreOut - radiated_);
        const double wave = boreOut - radiated_;
        boreLevel_ += follow * (wave * wave - boreLevel_);
        const double amplitude = std::sqrt(2.0 * boreLevel_) + 1.0e-12;
        const double drive = std::max(envelope_ * labiumRegister * (labiumSoft + (labiumLoud - labiumSoft) * std::pow(std::min(breath, 1.0), 2.0)), labiumStep);
        const double split = amplitude * (std::tanh(drive * wave / amplitude + labiumOffset) - std::tanh(labiumOffset)) / labiumFundamental(drive);
        const double radiated = formantGain * biquad(fall, biquad(rise, split, formant_[0], formant_[1]), formant_[2], formant_[3]);
        airLow_ += air * (white - airLow_);
        airHigh_ += airFloor * (airLow_ - airHigh_);
        const double sample = (radiated + 0.03 * noiseLevel * jetSpeed * airGain * (airLow_ - airHigh_)) * volume_;
        level_ = std::max(std::abs(sample), level_ * 0.9995);
        // Players blow high notes harder (Fletcher: pressure ~ frequency) and the open end radiates
        // them better: the level rises 3 dB an octave, so the third octave rings out as a flute's does.
        const double gain = output * expression_ * std::sqrt(fs / period_ / 261.6);
        left[frame] = static_cast<float>(gain * sample * gainLeft);
        right[frame] = static_cast<float>(gain * sample * gainRight);
    }
    frame_ = 0;
    if (!keyDown_ && envelope_ < 1.0e-4 && level_ < 1.0e-6) {
        endNote(note_);
        sounding_ = false;
    }
}

} // namespace pianocraft
