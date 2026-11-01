#include "engine/ReedEngine.hpp"
#include "engine/Filters.hpp"

#include <algorithm>
#include <cmath>

namespace pianocraft {

namespace {

using namespace filters;

// The B-flat clarinet sounds D3 to B-flat 6; the oboe B-flat 3 to A6. Constants set by ear and by
// tests/orchestra_tests.cpp (pitch, register, spectra by dynamic).
constexpr ReedModel clarinet{50, 94, false, 0.0, 0.7, -0.3, 0.5, 700.0, {{{1500.0, 2.0, 0.0}, {3000.0, 2.0, 0.0}}}, 0.0,
                             0.70, 0.78, 1.2, 6.0, 0.0, 3.5};
constexpr ReedModel oboe{58, 93, true, 0.3, 0.7, -0.3, 0.5, 1000.0, {{{1100.0, 2.5, 0.5}, {2800.0, 3.0, 0.3}}}, 0.6,
                         0.70, 0.80, 2.0, 8.0, 4.0, 2.5};

} // namespace

ReedEngine::ReedEngine(const ReedModel& model) : MonoWind(model.lowestKey, model.highestKey), model_(model) {
    parameters_[static_cast<std::size_t>(ReedParameterId::vibratoDepth)] = model.vibrato;
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
    noise_ = 0x2545F491u;
}

void ReedEngine::silence() {
    bore_ = {};
    apex_ = {};
    reflection_ = 0.0;
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
    const double pole = std::pow(std::clamp(model_.loss - 0.15 * (brightness - 1.0), 0.05, 0.9), rateScale);
    const double noiseLevel = std::max(parameter(ReedParameterId::breathNoise), 0.0);
    const double toneHoles = 1.0 - std::exp(-2.0 * pi * model_.toneHoles / fs);
    const auto first = bandpass(model_.formants[0][0], model_.formants[0][1], fs);
    const auto second = bandpass(model_.formants[1][0], model_.formants[1][1], fs);
    const double gainLeft = std::min(1.0, 2.0 * (1.0 - pan_));
    const double gainRight = std::min(1.0, 2.0 * pan_);
    const double output = 0.3 * model_.level * parameter(ReedParameterId::outputGain);
    const std::size_t mask = delaySize - 1;
    const double frequency = fs / period_ * bendFactor;
    const double tuning = std::pow(2.0, -model_.stretch * frequency * frequency / 1.0e6 / 1200.0);
    const double height = std::clamp((note_.key - model_.lowestKey) / static_cast<double>(model_.highestKey - model_.lowestKey), 0.0, 1.0);
    const double pressure = model_.pressureLow + (model_.pressureHigh - model_.pressureLow) * height;
    // A cone's reed and staple are a fixed length, so the higher the note, the larger their share of
    // the vibrating column: the reed sits further along it, and the fundamental speaks more.
    const double apex = std::min(model_.apex * std::pow(frequency / (440.0 * std::pow(2.0, (model_.lowestKey - 69) / 12.0)), 0.75), 0.35);
    // The dynamic: how far the player lets the reed open sets the level (Dalmont and Kergomard's
    // elementary model), and at soft dynamics the upper partials fall away (Benade): nearly pure at
    // pp, every partial at ff.
    const double dynamicBreath = std::clamp(breath_, 0.0, 1.0);
    const auto dynamic = butterworth(false, std::min(frequency * (model_.open + model_.bloom * dynamicBreath * dynamicBreath), 0.45 * fs), fs);

    for (std::uint32_t frame = 0; frame < frameCount; ++frame) {
        frame_ = frame;
        const Breath player = advance(pacing);
        const double vibrato = player.vibrato;
        const double breath = std::max(player.breath * (1.0 + 0.05 * vibrato), 0.0);
        noise_ ^= noise_ << 13;
        noise_ ^= noise_ >> 17;
        noise_ ^= noise_ << 5;
        noiseLow_ += 0.3 * (static_cast<double>(noise_) / 2147483648.0 - 1.0 - noiseLow_);
        // CC1 is the mouth's pressure behind the reed; the breath is turbulent, more so as the
        // tongue releases it and while a slur's new resonance takes over.
        const double mouth = player.envelope * pressure * (1.0 - 0.1 * player.slur) *
                             (1.0 + 0.02 * noiseLow_ * (noiseLevel + 4.0 * std::exp(-noteSeconds_ / 0.02) + 2.0 * player.slur));

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
        // While a slur's keys move, the loop takes the new length half way (a pad seating,
        // player.seat), and what radiates crossfades from the old length's reading to the new: the
        // pitches between hardly sound.
        const double k = player.keys;
        const double fromOld = k < 1.0 ? boreAt(periodFrom_ * tuned) : 0.0;
        const double toNew = boreAt(period_ * tuned);
        const double boreOut = k < 1.0 ? (1.0 - player.seat) * fromOld + player.seat * toNew : toNew;
        const double heard = k < 1.0 ? (1.0 - k) * fromOld + k * toNew : toNew;
        reflection_ = (1.0 - pole) * boreOut + pole * reflection_;
        const double fromBell = -0.95 * reflection_;
        // A cone (Scavone's form, as STK's Saxofony): the bell's wave also runs to the missing tip
        // and back, inverted there, and the reed hears both.
        const double fromTip = !model_.conical ? 0.0
                               : -0.99 * (k < 1.0 ? (1.0 - player.seat) * tipAt(periodFrom_ * tuned) + player.seat * tipAt(period_ * tuned)
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
        // the bell and body add their resonances.
        radiated_ += toneHoles * (heard - radiated_);
        const double holes = heard - radiated_;
        const double sound = holes + model_.formants[0][2] * biquad(first, holes, formant_[0], formant_[1]) +
                             model_.formants[1][2] * biquad(second, holes, formant_[2], formant_[3]);
        const double sample = biquad(dynamic, sound, dynamic_[0], dynamic_[1]) * std::pow(breath, 0.9) * volume_;
        level_ = std::max(std::abs(sound), level_ * 0.9995); // the reed speaking, whatever the dynamic
        const double gain = output * expression_;
        left[frame] = static_cast<float>(gain * sample * gainLeft);
        right[frame] = static_cast<float>(gain * sample * gainRight);
    }
    finishBlock();
}

} // namespace pianocraft
