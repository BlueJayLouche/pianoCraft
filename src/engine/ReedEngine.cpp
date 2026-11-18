#include "engine/ReedEngine.hpp"
#include "engine/Filters.hpp"

#include <algorithm>
#include <cmath>

namespace pianocraft {

namespace {

using namespace filters;

// The B-flat clarinet sounds D3 to B-flat 6; the oboe B-flat 3 to A6. The bores' constants set by ear
// and by tests/orchestra_tests.cpp (pitch, register, spectra by dynamic); the clarinet's even
// harmonics, the output stages, the breath's turbulence and wander fitted to the University of Iowa
// recordings (docs/oboe-and-clarinet.md, tools/iowa_calibration.py).
constexpr ReedModel clarinet{50, 94, false, 0.0, 0.7, -0.3, 0.5, 700.0, {{{1500.0, 2.0, 0.0}, {3000.0, 2.0, 0.0}}}, {}, 0.0,
                             0.70, 0.78, 1.2, 6.0, 0.0, 3.5, 0.3, {0, 1000.0, 428.0, 30.0, 0.7, 1.0}, 4.0, 1.0, 0.35, 0.0,
                             79, 0.0, 2.0, 0.0};
// The oboe's formants and tip peak fitted to the recordings' harmonics (with the formants off, the
// model's spectrum, then the filters that best turn it into theirs); its 2.9 kHz formant to their
// share above 3 kHz.
constexpr ReedModel oboe{58, 93, true, 0.3, 0.7, -0.3, 0.5, 1000.0, {{{1305.0, 5.0, 16.0}, {2900.0, 2.0, 6.0}}}, {0.99, 3.2, 12.5}, 0.6,
                         0.70, 0.80, 2.0, 8.0, 4.0, 1.57, 0.0, {1, 1129.0, 10463.0, 1.10, 0.0, 1.81}, 3.0, 0.2, 0.1, -3.0,
                         81, 1.0, 0.0, 4.4};

} // namespace

ReedEngine::ReedEngine(const ReedModel& model) : MonoWind(model.lowestKey, model.highestKey), model_(model) {
    parameters_[static_cast<std::size_t>(ReedParameterId::vibratoDepth)] = model.vibrato;
    wander_ = model.wander;
    attackScale_ = model.attack; // the oboe's tongue releases pressure already built behind it,
    puff_ = model.puff;          // without the flute's puff, which a breath this quick would follow
    registerInterval_ = model.conical ? 12 : 19; // a cylinder closed at the reed overblows a twelfth
}

OboeEngine::OboeEngine() : ReedEngine(oboe) {}

ClarinetEngine::ClarinetEngine() : ReedEngine(clarinet) {}

void ReedEngine::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    reset();
}

void ReedEngine::reset() {
    resetPlayer();
    silence();
    write_ = 0;
    radiated_ = noiseLow_ = 0.0;
    formant_ = {};
    dynamic_ = {};
    voicing_.reset();
    evenMean_ = 0.0;
    evenState_ = {};
    steady_ = 0.0;
    noise_ = 0x2545F491u;
}

void ReedEngine::silence() {
    bore_ = {};
    apex_ = {};
    reflection_ = 0.0;
    steady_ = 0.0;
}

void ReedEngine::render(float* left, float* right, std::uint32_t frameCount) {
    if (silent(left, right, frameCount)) {
        return;
    }
    const double fs = sampleRate_;
    const double rateScale = 48000.0 / fs;
    const Pace pacing = pace();
    const double bendFactor = std::pow(2.0, 2.0 * bend_ / 12.0);
    // The bell's losses: a one-pole low-pass in the reflection, darker at low Brightness.
    const double brightness = std::clamp(parameter(ReedParameterId::brightness) + brightnessModulation_ + timbre_, 0.25, 3.0);
    // Above the top register's first note a bore's losses shrink with its length (a shorter tube
    // loses less of its high partials each round trip); below it the fitted loss holds.
    const double shorter = std::pow(std::min(period_ / period(model_.topKey), 1.0), model_.topLoss);
    const double pole = std::pow(std::clamp((model_.loss - 0.15 * (brightness - 1.0)) * shorter, 0.05, 0.9), rateScale);
    const double noiseLevel = std::max(parameter(ReedParameterId::breathNoise), 0.0);
    const double toneHoles = 1.0 - std::exp(-2.0 * pi * model_.toneHoles / fs);
    // Reed: how much the reed buzzes. Up to 1 it raises the oboe's formants (in dB) and the clarinet's
    // even harmonics from none; above 1 the formants also widen, so the buzz spreads across them
    // rather than picking out whichever harmonic lands on a peak.
    const double reedAmount = std::clamp(parameter(ReedParameterId::reed), 0.0, 2.0);
    const double raise = std::min(reedAmount, 1.0) + 0.25 * std::max(reedAmount - 1.0, 0.0);
    const double widen = std::pow(std::max(reedAmount, 1.0), 1.5);
    const auto first = peaking(model_.formants[0][0], model_.formants[0][1] / widen, raise * model_.formants[0][2], fs);
    const auto second = peaking(model_.formants[1][0], model_.formants[1][1] / widen, raise * model_.formants[1][2], fs);
    const double gainLeft = std::min(1.0, 2.0 * (1.0 - pan_));
    const double gainRight = std::min(1.0, 2.0 * pan_);
    const double output = 0.3 * model_.level * parameter(ReedParameterId::outputGain);
    const std::size_t mask = delaySize - 1;
    const double frequency = fs / period_ * bendFactor;
    // The top register's shorter losses run less flat, so less is taken back above its first note.
    const double topFrequency = fs / period(model_.topKey) * bendFactor;
    const double above = std::max(frequency * frequency - topFrequency * topFrequency, 0.0);
    const double tuning = std::pow(2.0, -(model_.stretch * frequency * frequency - model_.topStretch * above) / 1.0e6 / 1200.0);
    const double height = std::clamp((note_.key - model_.lowestKey) / static_cast<double>(model_.highestKey - model_.lowestKey), 0.0, 1.0);
    const double pressure = model_.pressureLow + (model_.pressureHigh - model_.pressureLow) * height;
    // A cone's reed and staple are a fixed length, so the higher the note, the larger their share of
    // the vibrating column: the reed sits further along it, and the fundamental speaks more.
    const double apex = std::min(model_.apex * std::pow(frequency / (440.0 * std::pow(2.0, (model_.lowestKey - 69) / 12.0)), 0.75), 0.35);
    // A cone's tip peak (the clarinet's bore needs none).
    const auto tip = model_.conical ? peaking(model_.tip[0] * frequency / apex, model_.tip[1], model_.tip[2], fs) : std::array<double, 5>{1.0};
    // The peaks lift a note whose first two harmonics they reach; fitted to the recordings' levels,
    // note by note, three quarters of that power is taken back.
    const auto lift = [&](double f) {
        const double omega = 2.0 * pi * std::min(f, 0.45 * fs) / fs;
        const double m = magnitude(first, omega) * magnitude(second, omega) * magnitude(tip, omega);
        return m * m;
    };
    // And Reed keeps the loudness about where it was (measured across the range).
    const double levelling = std::pow(0.5 * (lift(frequency) + lift(2.0 * frequency)), -0.375) * std::pow(10.0, model_.reedLevel * (reedAmount - 1.0) / 20.0);
    // The dynamic: how far the player lets the reed open sets the level (Dalmont and Kergomard's
    // elementary model), and at soft dynamics the upper partials fall away (Benade): nearly pure at
    // pp, every partial at ff.
    const double dynamicBreath = std::clamp(breath_, 0.0, 1.0);
    const auto dynamic = butterworth(false, std::min(frequency * (model_.open + model_.bloom * dynamicBreath * dynamicBreath), 0.45 * fs), fs);
    voicing_.block(model_.voicing, dynamicBreath, frequency, fs);
    const double gateRate = 1.0 - std::exp(-1.0 / (0.001 * fs)); // the gate moves over 1 ms, so it never clicks
    const double evenRate = 1.0 - std::exp(-2.0 * pi * 10.0 / fs);
    const auto evenCutoff = butterworth(true, 1500.0, fs);
    // In the top register more of the harmonics lie above the tone holes' cutoff: the even ones grow.
    const double topOctaves = std::max(note_.key - model_.topKey, 0) / 12.0;
    const double evensUp = 1.0 + model_.topEvens * topOctaves * topOctaves;

    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        frame_ = frame;
        const Breath player = advance(pacing);
        const double vibrato = player.vibrato;
        const double breath = std::max(player.breath * (1.0 + 0.05 * vibrato), 0.0);
        noise_ ^= noise_ << 13;
        noise_ ^= noise_ >> 17;
        noise_ ^= noise_ << 5;
        noiseLow_ += 0.3 * (static_cast<double>(noise_) / 2147483648.0 - 1.0 - noiseLow_);
        // The mouth's pressure behind the reed, steady per note; the breath is turbulent (20% of the
        // noise, fitted to the recordings' noise between harmonics), more so as the tongue releases
        // it and while a slur's new resonance takes over.
        const double mouth = player.envelope * pressure * (1.0 - 0.1 * player.slur) *
                             (1.0 + noiseLow_ * (0.2 * noiseLevel + 0.02 * (4.0 * std::exp(-noteSeconds_ / 0.02) + 2.0 * player.slur)));

        // The bore. A cylinder's round trip is half the period (the bell inverts the wave); a cone,
        // open at both ends, takes the whole period over its two sides. Less the bell filter's phase
        // delay at the fundamental; the oboe's loop runs flat at the top, which `stretch` takes back.
        const double tuned = tuning / bendFactor * (1.0 - 0.0025 * vibrato);
        // The bore at a given period's length, and the cone's tip; while a slur's keys move, the reed
        // hears both the old length and the new.
        const auto boreAt = [&](double cycle) {
            const double tip = apex * cycle;
            const double delay = (model_.conical ? cycle - tip : 0.5 * cycle) - phaseDelay(1.0 - pole, 0.0, pole, 2.0 * pi / cycle);
            return lagrange(bore_.data(), mask, write_, std::clamp(delay, 4.0, static_cast<double>(delaySize - 4)));
        };
        const auto tipAt = [&](double cycle) { return lagrange(apex_.data(), mask, write_, std::max(apex * cycle, 2.0)); };
        // While a slur's keys move, the reed hears the bore at both lengths, in the shares the pads
        // give them (player.seat), and the half-open holes lose more of the wave (player.leak).
        const bool slurring = player.keys < 1.0;
        const double toNew = boreAt(period_ * tuned);
        const double heard = slurring ? (1.0 - player.seat) * boreAt(player.from * tuned) + player.seat * boreAt(player.to * tuned) : toNew;
        reflection_ = (1.0 - pole) * heard + pole * reflection_;
        const double fromBell = -0.95 * (1.0 - leakLoss * player.leak) * reflection_;
        // A cone (Scavone's form, as STK's Saxofony): the bell's wave also runs to the missing tip
        // and back, inverted there, and the reed hears both.
        const double fromTip = !model_.conical ? 0.0
                               : -0.99 * (slurring ? (1.0 - player.seat) * tipAt(player.from * tuned) + player.seat * tipAt(player.to * tuned)
                                                   : tipAt(period_ * tuned));

        // The reed (J. O. Smith's reed table): the pressure across it closes it, so the bore's
        // returning wave is reflected by the closed reed and the mouth's pressure let in through the
        // open one.
        const double arriving = fromBell + fromTip;
        const double difference = arriving - mouth;
        const double reed = std::clamp(model_.reedOffset + model_.reedSlope * difference, -1.0, 1.0);
        const double leaving = mouth + difference * reed;
        if (model_.conical) {
            bore_[write_] = leaving - fromBell;
            apex_[write_] = fromBell / 0.99;
        } else {
            bore_[write_] = std::abs(leaving) < 1.0e-30 ? 0.0 : leaving;
        }
        write_ = (write_ + 1) & mask;

        // The tone holes radiate the bore's pressure, rising 6 dB an octave up to their cutoff;
        // the bell and body add their resonances, and a cone's tip peak puts back what the model's tip
        // cancelled.
        radiated_ += toneHoles * (heard - radiated_);
        double holes = heard - radiated_;
        if (model_.evens > 0.0) {
            const double square = holes * holes;
            evenMean_ += evenRate * (square - evenMean_);
            holes += reedAmount * model_.evens * evensUp * biquad(evenCutoff, (square - evenMean_) / std::sqrt(evenMean_ + 1.0e-20), evenState_[0], evenState_[1]);
        }
        // The peaks shape the reed's steady regime, where the model's tip cancels: the bore's
        // transients (the note settling, a slur's keys moving, the breath falling away) carry
        // that partial already, and would squawk with it lifted again. So they come in as the
        // note settles and leave as the breath falls away; a slur takes half of them (all, and the
        // pitches between its notes smear).
        const double shaped = biquad(second, biquad(first, biquad(tip, holes, formant_[4], formant_[5]), formant_[0], formant_[1]),
                                     formant_[2], formant_[3]) * levelling;
        const double held = player.envelope * player.envelope;
        const double steady = (1.0 - std::exp(-noteSeconds_ / 0.15)) * std::min(held * held, 1.0) * (1.0 - 0.5 * player.slur);
        steady_ += gateRate * (steady - steady_);
        const double sound = holes + steady_ * (shaped - holes);
        const double sample = voicing_.process(biquad(dynamic, sound, dynamic_[0], dynamic_[1])) * std::pow(breath, 0.9) * volume_;
        level_ = std::max(std::abs(sound), level_ * 0.9995); // the reed speaking, whatever the dynamic
        const double gain = output * expression_;
        left[frame] = static_cast<float>(gain * sample * gainLeft);
        right[frame] = static_cast<float>(gain * sample * gainRight);
    }
    finishBlock();
}

} // namespace pianocraft
