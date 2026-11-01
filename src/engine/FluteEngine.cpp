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

FluteEngine::FluteEngine() : MonoWind(lowestKey, highestKey) {}

void FluteEngine::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    reset();
}

void FluteEngine::reset() {
    resetPlayer();
    jetLow_ = 0.0;
    bore_ = {};
    jet_ = {};
    write_ = 0;
    reflection_ = dcInput_ = dcOutput_ = radiated_ = noiseLow_ = airLow_ = airHigh_ = 0.0;
    formant_ = {};
    boreLevel_ = 0.0;
    noise_ = 0x9E3779B9u;
}

void FluteEngine::silence() {
    bore_ = {};
    jet_ = {};
}

void FluteEngine::render(float* left, float* right, std::uint32_t frameCount) {
    const auto& p = parameters_;
    if (silent(left, right, frameCount)) {
        return;
    }
    const double fs = sampleRate_;
    const double rateScale = 48000.0 / fs;
    // The open end loses treble: a one-pole low-pass, darker at low Brightness.
    const double brightness = std::clamp(p[index(FluteParameterId::brightness)] + brightnessModulation_ + timbre_, 0.25, 3.0);
    const double pole = std::pow(std::clamp(0.45 - 0.15 * (brightness - 1.0), 0.1, 0.8), rateScale);
    const double dcPole = std::pow(0.9995, rateScale);
    const Pace pacing = pace();
    const double bendFactor = std::pow(2.0, 2.0 * bend_ / 12.0);
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
        const Breath player = advance(pacing);
        // While a slur's new resonance takes over, the jet is a little less steady and noisier.
        const double slur = player.slur;
        const double vibrato = player.vibrato;
        // CC1 sets the jet's speed; the pressure behind it is its square. Vibrato pulses that
        // pressure by +-10% (Fletcher), so it is mostly a vibrato of the upper partials.
        const double breath = std::max(player.breath * (1.0 + 0.05 * vibrato), 0.0);
        const double jetSpeed = envelope_ * std::pow(breath, fundamentalLaw) * (1.0 - 0.15 * slur);
        const double tuned = jetTuning * registerTuning / bendFactor * (1.0 - 0.0025 * vibrato);
        const double cycle = period_ * tuned;

        // Delays: the bore round trip less its filters' phase delay at the fundamental, and the
        // jet's flight over half a period less its own low-pass (corner 2 f0: the jet can't follow
        // the higher harmonics as well), which keeps it in the first register.
        const double omega = 2.0 * pi / cycle;
        const auto boreAt = [&](double length) {
            const double w = 2.0 * pi / length;
            const double loopDelay = phaseDelay(1.0 - pole, 0.0, pole, w) + phaseDelay(1.0, -1.0, dcPole, w);
            return lagrange(bore_.data(), delaySize - 1, write_, std::clamp(length - loopDelay, 4.0, static_cast<double>(delaySize - 4)));
        };
        const double jetPole = std::exp(-2.0 * omega);
        const double jetDelay = std::clamp(jetRatio * cycle - phaseDelay(1.0 - jetPole, 0.0, jetPole, omega), 1.0,
                                           static_cast<double>(delaySize - 2));

        // The bore, cubic Lagrange. While a slur's keys move, the loop takes the new length half way
        // (a pad seating, player.seat), and what radiates crossfades from the old length's reading to
        // the new: the pitches between hardly sound.
        const double fromOld = player.keys < 1.0 ? boreAt(periodFrom_ * tuned) : 0.0;
        const double toNew = boreAt(cycle);
        const double boreOut = player.keys < 1.0 ? (1.0 - player.seat) * fromOld + player.seat * toNew : toNew;
        const double heard = player.keys < 1.0 ? (1.0 - player.keys) * fromOld + player.keys * toNew : toNew;
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
        radiated_ += highpass * (heard - radiated_);
        const double wave = heard - radiated_;
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
    finishBlock();
}

} // namespace pianocraft
