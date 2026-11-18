#include "engine/PianoEngine.hpp"
#include "engine/SympatheticBank.hpp"
#include "model/CalibrationData.hpp"
#include "model/InstrumentModel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <utility>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<float> render(pianocraft::PianoEngine& engine, std::uint32_t frames, std::uint32_t blockSize) {
    std::vector<float> output;
    output.reserve(frames * 2);
    std::vector<float> left(blockSize);
    std::vector<float> right(blockSize);
    std::uint32_t rendered = 0;
    while (rendered < frames) {
        const auto count = std::min(blockSize, frames - rendered);
        engine.render(left.data(), right.data(), count);
        for (std::uint32_t index = 0; index < count; ++index) {
            require(std::isfinite(left[index]) && std::isfinite(right[index]), "Non-finite output");
            output.push_back(left[index]);
            output.push_back(right[index]);
        }
        rendered += count;
    }
    return output;
}

double rms(const std::vector<float>& samples) {
    if (samples.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (float sample : samples) {
        sum += static_cast<double>(sample) * sample;
    }
    return std::sqrt(sum / samples.size());
}

double peak(const std::vector<float>& samples) {
    double value = 0.0;
    for (float sample : samples) {
        value = std::max(value, std::abs(static_cast<double>(sample)));
    }
    return value;
}

pianocraft::NoteAddress note(int key, int id = -1) {
    return {id, 0, 0, static_cast<std::int16_t>(key)};
}

void modelIsStable() {
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        pianocraft::InstrumentModel model(rate);
        for (std::size_t region = 0; region < 3; ++region) {
            require(pianocraft::soundboardCalibration.directGain[region] >= 0.5 &&
                    pianocraft::soundboardCalibration.directGain[region] <= 1.5,
                    "Invalid soundboard direct gain");
            require(pianocraft::soundboardCalibration.resonanceGain[region] >= 0.0 &&
                    pianocraft::soundboardCalibration.resonanceGain[region] <= 2.0,
                    "Invalid soundboard resonance gain");
            require(std::abs(pianocraft::soundboardCalibration.spectralTilt[region]) <= 1.0,
                    "Invalid soundboard spectral tilt");
        }
        for (int key = 21; key <= 108; ++key) {
            const auto& value = model.key(key);
            require(value.leftGain > 0.0 && value.rightGain > 0.0, "Invalid stereo output gains");
            require(value.velocityGainExponent >= 0.4 && value.velocityGainExponent <= 2.5,
                    "Invalid velocity gain exponent");
            require(value.hammerMass > 0.004 && value.hammerMass < 0.012 && value.feltExponent >= 3.7 &&
                        value.feltExponent <= 5.1 && value.residualCompliance >= 0.0,
                    "Invalid hammer");
            require(value.modeCount > 0 && value.modeCount <= pianocraft::modeLimit, "Invalid mode count");
            for (std::size_t index = 0; index < value.modeCount; ++index) {
                const auto& mode = value.modes[index];
                require(mode.sustainA2 < 1.0 && mode.releaseA2 < 1.0, "Unstable pole");
                require(mode.frequency > 0.0 && mode.frequency < rate * 0.5, "Invalid modal frequency");
                require(std::isfinite(mode.excitation) && std::isfinite(mode.output), "Invalid coefficient");
            }
        }
    }
}

void noteProducesSoundAndReleases() {
    pianocraft::PianoEngine engine;
    engine.prepare(48000.0);
    require(rms(render(engine, 4800, 64)) == 0.0, "Initial silence expected");
    engine.noteOn(note(60), 0.8);
    const auto attack = render(engine, 48000, 64);
    require(peak(attack) > 1.0e-6, "Note is silent");
    engine.noteOff(note(60), 0.0);
    const auto release = render(engine, 48000 * 4, 64);
    const auto tail = render(engine, 48000 * 4, 64);
    require(rms(tail) < rms(release) && rms(tail) < rms(attack), "Release did not decay");
}

void sustainHoldsNotes() {
    pianocraft::PianoEngine engine;
    engine.prepare(48000.0);
    engine.setSustain(0, true);
    engine.noteOn(note(48), 0.7);
    render(engine, 24000, 128);
    engine.noteOff(note(48), 0.0);
    const auto held = render(engine, 48000, 128);
    require(rms(held) > 1.0e-6, "Sustain did not hold note");
    engine.setSustain(0, false);
    render(engine, 48000 * 5, 128);
    const auto tail = render(engine, 48000 * 5, 128);
    require(rms(tail) < rms(held), "Pedal-up did not release note");
}

void restrikesAndPolyphonyAreFinite() {
    pianocraft::PianoEngine engine;
    engine.prepare(48000.0);
    for (int index = 0; index < 12; ++index) {
        engine.noteOn(note(40, index), 0.25 + 0.06 * index);
        render(engine, 480, 64);
        engine.noteOff(note(40, index), 0.0);
    }
    for (int key = 33; key < 97; ++key) {
        engine.noteOn(note(key, key), 0.65);
    }
    // Restrikes reuse their key's string whatever their note ids (hosts give each note a fresh id),
    // so the 12 strikes of key 40 and its later note share one voice: 64 voices for 64 keys.
    require(engine.activeVoiceCount() == 64, "Expected 64 active voices, got " + std::to_string(engine.activeVoiceCount()));
    const auto output = render(engine, 2400, 64);
    require(std::isfinite(peak(output)), "Dense polyphony failed");
    engine.reset();
    require(engine.activeVoiceCount() == 0, "Reset left voices active");
    require(peak(render(engine, 256, 64)) == 0.0, "Reset did not silence output");
}

// Nothing steps in a sample: a restrike under the pedal, or the pedal itself, leaves no kink. The
// waveform's sharpest third difference in the 0.5 ms from the event (before the hammer lands),
// against its median over the 50 ms before. A restruck string's gain and EQ once stepped: 1000x.
void nothingClicks() {
    const auto kink = [](const std::vector<float>& stereo, std::size_t at) {
        std::vector<double> third;
        for (std::size_t i = at - 2400; i < at - 24; ++i) {
            third.push_back(std::abs(stereo[2 * (i + 3)] - 3.0 * stereo[2 * (i + 2)] + 3.0 * stereo[2 * (i + 1)] - stereo[2 * i]));
        }
        std::nth_element(third.begin(), third.begin() + third.size() / 2, third.end());
        double sharpest = 0.0;
        for (std::size_t i = at - 2; i < at + 24; ++i) {
            sharpest = std::max(sharpest, static_cast<double>(std::abs(stereo[2 * (i + 3)] - 3.0 * stereo[2 * (i + 2)] +
                                                                       3.0 * stereo[2 * (i + 1)] - stereo[2 * i])));
        }
        return sharpest / third[third.size() / 2];
    };
    for (const auto& strikes : {std::pair{0.8, 0.2}, {0.2, 0.8}}) {
        pianocraft::PianoEngine engine;
        engine.prepare(48000.0);
        engine.setSustain(0, 1.0);
        engine.noteOn(note(60, 1), strikes.first);
        auto output = render(engine, 9600, 64);
        engine.noteOff(note(60, 1), 0.0);
        const auto ringing = render(engine, 33600, 64);
        output.insert(output.end(), ringing.begin(), ringing.end());
        engine.noteOn(note(60, 2), strikes.second);
        const auto restruck = render(engine, 4800, 64);
        output.insert(output.end(), restruck.begin(), restruck.end());
        const double sharpest = kink(output, 43200);
        require(sharpest < 10.0, "A pedalled restrike clicked: " + std::to_string(sharpest) + "x");
    }
    pianocraft::PianoEngine engine;
    engine.prepare(48000.0);
    engine.setSustain(0, 1.0);
    for (int key : {48, 55, 60, 64, 67, 72, 76, 79, 84, 88}) {
        engine.noteOn(note(key, key), 0.8);
    }
    auto output = render(engine, 24000, 64);
    for (int key : {48, 55, 60, 64, 67, 72, 76, 79, 84, 88}) {
        engine.noteOff(note(key, key), 0.0);
    }
    const auto ringing = render(engine, 24000, 64);
    output.insert(output.end(), ringing.begin(), ringing.end());
    engine.setSustain(0, 0.0);
    const auto lifted = render(engine, 4800, 64);
    output.insert(output.end(), lifted.begin(), lifted.end());
    engine.setSustain(0, 1.0);
    const auto lowered = render(engine, 4800, 64);
    output.insert(output.end(), lowered.begin(), lowered.end());
    for (std::size_t at : {48000u, 52800u}) {
        const double sharpest = kink(output, at);
        require(sharpest < 10.0, "A square pedal step clicked: " + std::to_string(sharpest) + "x");
    }
}

// The pedal sounds by its speed: a stamp knocks, a slow move barely (its knock, without the strings'
// shimmer, which gathers over a slow lift), Pedal Noise 0 not at all, and the noise grows from
// nothing rather than starting in a sample.
void pedalMakesNoise() {
    const auto press = [](double noise, int steps, double sympathy = 0.35) {
        pianocraft::PianoEngine engine;
        engine.prepare(48000.0);
        engine.setParameter(pianocraft::ParameterId::pedalNoise, noise);
        engine.setParameter(pianocraft::ParameterId::sympatheticLevel, sympathy);
        std::vector<float> output;
        for (int step = 1; step <= steps; ++step) {
            engine.setSustain(0, static_cast<double>(step) / steps);
            const auto part = render(engine, 14400 / steps, 64);
            output.insert(output.end(), part.begin(), part.end());
        }
        const auto rest = render(engine, 9600, 64);
        output.insert(output.end(), rest.begin(), rest.end());
        return output;
    };
    const auto stamp = press(1.0, 1);
    require(peak(stamp) > 1.0e-5, "A pedal stamp made no sound");
    require(peak(press(0.0, 1)) == 0.0, "Pedal Noise 0 still sounded");
    const double knock = peak(press(1.0, 1, 0.0));
    const double slowKnock = peak(press(1.0, 30, 0.0)); // 300 ms down
    require(slowKnock < 0.18 * knock, "A slow pedal knocked within 15 dB of a stamp");
    const std::vector<float> onset(stamp.begin(), stamp.begin() + 24);
    require(peak(onset) < 0.05 * peak(stamp), "The pedal noise started in a sample"); // 2.6% by 0.5 ms
}

// A re-render must line up with the one it replaces: the same events give the same samples at any
// host block size, as the CLAP host splits blocks at each event.
void blockSizesAreDeterministic() {
    using pianocraft::ParameterId;
    const auto play = [](double rate, std::uint32_t blockSize) {
        const auto at = [rate](double seconds) { return static_cast<std::uint32_t>(seconds * rate); };
        pianocraft::PianoEngine engine;
        engine.prepare(rate);
        const std::vector<std::pair<std::uint32_t, std::function<void()>>> events{
            {at(0.0), [&] { engine.noteOn(note(60), 0.75); engine.noteOn(note(36), 0.5); }},
            {at(0.03), [&] { engine.setParameter(ParameterId::sustainLength, 2.0); }},
            {at(0.05), [&] { engine.setSustain(0, 0.6); }},
            {at(0.07), [&] { engine.setParameter(ParameterId::micPosition, 2.0); }},
            {at(0.09), [&] { engine.setParameter(ParameterId::stereoWidth, 0.0); }},
            {at(0.11), [&] { engine.modulate({-1, -1, -1, 60}, ParameterId::hammerHardness, 0.5); }},
            {at(0.12), [&] { engine.noteOff(note(60), 0.0); engine.noteOn(note(84), 1.0); }},
            {at(0.20), [&] { engine.setSustain(0, 0.0); }},
        };
        std::vector<float> output;
        std::uint32_t rendered = 0;
        for (std::size_t index = 0; index <= events.size(); ++index) {
            const std::uint32_t until = index < events.size() ? events[index].first : at(0.3);
            const auto segment = render(engine, until - rendered, blockSize);
            output.insert(output.end(), segment.begin(), segment.end());
            rendered = until;
            if (index < events.size()) {
                events[index].second();
            }
        }
        return output;
    };
    for (double rate : {48000.0, 96000.0}) {
        const auto reference = play(rate, 64);
        require(play(rate, 127) == reference && play(rate, 1024) == reference,
                "Output depends on host block size at " + std::to_string(rate) + " Hz");
        require(play(rate, 64) == reference, "Output differs between identical renders");
    }
}

void parametersAndRatesAreValid() {
    pianocraft::PianoEngine engine;
    // 1234.57 Hz puts treble fundamentals and the 1 kHz velocity shelf above Nyquist.
    for (double rate : {1234.57, 44100.0, 48000.0, 96000.0, 768000.0}) {
        engine.prepare(rate);
        engine.setParameter(pianocraft::ParameterId::outputGain, 99.0);
        require(engine.parameterValue(pianocraft::ParameterId::outputGain) == 2.0, "Parameter clamp failed");
        engine.setParameter(pianocraft::ParameterId::outputGain, 0.7);
        engine.noteOn(note(69), 0.0);
        require(peak(render(engine, static_cast<std::uint32_t>(rate / 10), 64)) > 0.0,
                "Native zero-velocity note-on must remain audible");
        engine.reset();
        engine.setSustain(0, true);
        for (int key = 21; key <= 108; key += 5) {
            engine.noteOn(note(key), 1.0);
        }
        render(engine, static_cast<std::uint32_t>(rate / 4) + 64, 64);  // render() rejects non-finite output
        engine.reset();
    }
}

void sympatheticIsSelective() {
    pianocraft::InstrumentModel model(48000.0);
    pianocraft::SympatheticBank bank;
    bank.prepare(48000.0, model);
    std::array<double, pianocraft::instrumentKeyCount> undamped{};
    std::array<double, 3> input{0.0, 1.0, 0.0};
    double dampedEnergy = 0.0;
    for (std::size_t frame = 0; frame < 4096; ++frame) {
        dampedEnergy += std::abs(bank.process(input, undamped, 1.0)[1]);
        input[1] = 0.0;
    }

    bank.reset();
    undamped[60 - 21] = true;
    input = {0.0, 1.0, 0.0};
    double openEnergy = 0.0;
    for (std::size_t frame = 0; frame < 4096; ++frame) {
        openEnergy += std::abs(bank.process(input, undamped, 1.0)[1]);
        input[1] = 0.0;
    }
    require(openEnergy > dampedEnergy, "Open key did not increase sympathetic response");

    undamped.fill(false);
    double releasedEnergy = 0.0;
    for (std::size_t frame = 0; frame < 96000; ++frame) {
        const auto output = bank.process(input, undamped, 1.0)[1];
        if (frame >= 48000) {
            releasedEnergy += std::abs(output);
        }
    }
    require(releasedEnergy < openEnergy * 0.01, "Damped sympathetic response did not release");
    require(bank.process(input, undamped, 0.0)[1] == 0.0, "Zero sympathy level produced output");

    // The board is shared: a treble-region drive must reach an open bass string.
    bank.reset();
    undamped.fill(false);
    undamped[36 - 21] = true;
    input = {0.0, 0.0, 1.0};
    double bassEnergy = 0.0;
    for (std::size_t frame = 0; frame < 4096; ++frame) {
        bassEnergy += std::abs(bank.process(input, undamped, 1.0)[0]);
        input[2] = 0.0;
    }
    require(bassEnergy > 0.0, "Treble drive did not reach an open bass string");
}

void bodyAndSympatheticAffectOutput() {
    pianocraft::PianoEngine dry;
    pianocraft::PianoEngine resonant;
    dry.prepare(48000.0);
    resonant.prepare(48000.0);
    dry.setSustain(0, true);
    resonant.setSustain(0, true);
    dry.setParameter(pianocraft::ParameterId::sympatheticLevel, 0.0);
    resonant.setParameter(pianocraft::ParameterId::sympatheticLevel, 1.0);
    dry.noteOn(note(36), 0.8);
    resonant.noteOn(note(36), 0.8);
    const auto a = render(dry, 24000, 64);
    const auto b = render(resonant, 24000, 64);
    require(a != b, "Sympathetic level did not affect output");
    require(peak(a) < 0.5 && peak(b) < 0.5,
            "Unexpected clipping in body paths: dry=" + std::to_string(peak(a)) +
            " resonant=" + std::to_string(peak(b)));
}

void trebleIsBalancedAndClear() {
    const auto play = [](std::int16_t key) {
        pianocraft::PianoEngine engine;
        engine.prepare(48000.0);
        engine.noteOn(note(key), 0.63);
        return render(engine, 24000, 64);
    };
    const auto c3 = play(48);
    const auto c6 = play(84);
    const auto c7 = play(96);
    require(rms(c7) > rms(c3) * 0.1, "Treble is more than 20 dB below the middle register: C3=" +
            std::to_string(rms(c3)) + " C7=" + std::to_string(rms(c7)));

    // Two one-pole low-passes at 200 Hz; this band once held most of C6's energy (ratio ~0.9,
    // soundboard mud). It is ~0.3 now.
    const double coefficient = 1.0 - std::exp(-2.0 * 3.141592653589793 * 200.0 / 48000.0);
    double stage1 = 0.0;
    double stage2 = 0.0;
    double lowEnergy = 0.0;
    for (std::size_t index = 0; index < c6.size(); index += 2) {
        stage1 += coefficient * (c6[index] - stage1);
        stage2 += coefficient * (stage1 - stage2);
        lowEnergy += stage2 * stage2;
    }
    const double lowRms = std::sqrt(lowEnergy / (c6.size() / 2));
    require(lowRms < rms(c6) * 0.5, "C6 is dominated by energy below 200 Hz: ratio=" +
            std::to_string(lowRms / rms(c6)));
}

double correlation(const std::vector<float>& output) {
    double cross = 0.0;
    double leftEnergy = 0.0;
    double rightEnergy = 0.0;
    for (std::size_t index = 0; index < output.size(); index += 2) {
        cross += static_cast<double>(output[index]) * output[index + 1];
        leftEnergy += static_cast<double>(output[index]) * output[index];
        rightEnergy += static_cast<double>(output[index + 1]) * output[index + 1];
    }
    return cross / std::sqrt(leftEnergy * rightEnergy);
}

void notesSustainInStereo() {
    pianocraft::PianoEngine engine;
    engine.prepare(48000.0);
    engine.setSustain(0, true);
    engine.noteOn(note(84), 0.63);
    const auto output = render(engine, 72000, 64);
    const std::vector<float> attack(output.begin(), output.begin() + 9600);
    const std::vector<float> tail(output.begin() + 96000, output.end());
    // C6 used to fall silent within a second; the recordings are ~-25 dB at 1-1.5 s.
    require(rms(tail) > rms(attack) * 0.01, "C6 does not sustain: tail/attack=" +
            std::to_string(rms(tail) / rms(attack)));

    const double stereo = correlation(output);
    require(stereo < 0.9, "Output is effectively mono: L/R correlation=" + std::to_string(stereo));
}

void micPositionsAndWidth() {
    const auto play = [](double mic, double width, bool switchAfterPrepare) {
        pianocraft::PianoEngine engine;
        // Width is smoothed like output gain; setting it before prepare starts it settled.
        engine.setParameter(pianocraft::ParameterId::stereoWidth, width);
        if (!switchAfterPrepare) {
            engine.setParameter(pianocraft::ParameterId::micPosition, mic);
        }
        engine.prepare(48000.0);
        if (switchAfterPrepare) {
            engine.setParameter(pianocraft::ParameterId::micPosition, mic);
        }
        engine.noteOn(note(72), 0.63);
        return render(engine, 48000, 64);
    };
    const double spaced = correlation(play(0.0, 1.0, false));
    const double coincident = correlation(play(1.0, 1.0, false));
    const double player = correlation(play(2.0, 1.0, false));
    require(coincident > 0.9, "Coincident pair is not phase-coherent: " + std::to_string(coincident));
    require(spaced < coincident && player < coincident,
            "Spaced/player mics are not wider than coincident: " + std::to_string(spaced) + " " +
            std::to_string(player));
    require(play(1.0, 1.0, true) == play(1.0, 1.0, false), "Mic change after prepare differs from before");

    const auto mono = play(0.0, 0.0, false);
    for (std::size_t index = 0; index < mono.size(); index += 2) {
        require(std::abs(mono[index] - mono[index + 1]) < 1.0e-6f, "Zero width is not mono");
    }

    // A mono bounce sums the channels; no partial may notch against one channel alone. C3's first
    // 24 partials, 0.1-0.6 s, Goertzel per partial.
    const pianocraft::InstrumentModel model(48000.0);
    const auto& key = model.key(48);
    const auto deepestNotch = [&](double mic, double width) {
        pianocraft::PianoEngine engine;
        engine.setParameter(pianocraft::ParameterId::stereoWidth, width);
        engine.setParameter(pianocraft::ParameterId::micPosition, mic);
        engine.prepare(48000.0);
        engine.noteOn(note(48), 0.63);
        const auto output = render(engine, 28800, 64);
        const auto magnitude = [&](double frequency, bool sum) {
            const double omega = 2.0 * 3.141592653589793 * frequency / 48000.0;
            double real = 0.0, imaginary = 0.0;
            for (std::size_t frame = 4800; frame < 28800; ++frame) {
                const double x = sum ? 0.5 * (output[2 * frame] + output[2 * frame + 1]) : output[2 * frame];
                real += x * std::cos(omega * frame);
                imaginary += x * std::sin(omega * frame);
            }
            return std::hypot(real, imaginary);
        };
        double deepest = 0.0;
        for (std::size_t index = 0; index < 24; ++index) {
            const double frequency = key.modes[index].frequency;
            deepest = std::min(deepest, 20.0 * std::log10(magnitude(frequency, true) / magnitude(frequency, false)));
        }
        return deepest;
    };
    for (double mic : {0.0, 1.0, 2.0}) {
        const double notch = deepestNotch(mic, 0.0);
        require(notch > -1.0, "Mono sum notches at mic " + std::to_string(mic) + ": " + std::to_string(notch) + " dB");
    }
    require(deepestNotch(0.0, 1.0) < -6.0, "Notch check would not catch the spaced pair's comb filtering");
}

double trebleShare(const std::vector<float>& output) {
    // Energy above ~2 kHz (two one-pole high-passes) relative to the whole signal.
    const double coefficient = std::exp(-2.0 * 3.141592653589793 * 2000.0 / 48000.0);
    double in1 = 0.0, out1 = 0.0, in2 = 0.0, out2 = 0.0, high = 0.0, total = 0.0;
    for (std::size_t index = 0; index < output.size(); index += 2) {
        const double x = output[index];
        out1 = coefficient * (out1 + x - in1);
        in1 = x;
        out2 = coefficient * (out2 + out1 - in2);
        in2 = out1;
        high += out2 * out2;
        total += x * x;
    }
    return high / total;
}

void louderIsBrighter() {
    const auto play = [](double velocity) {
        pianocraft::PianoEngine engine;
        engine.prepare(48000.0);
        engine.noteOn(note(60), velocity);
        return render(engine, 19200, 64);
    };
    const double soft = trebleShare(play(0.3));
    const double loud = trebleShare(play(0.95));
    require(loud > soft * 2.0, "ff is not >3 dB brighter than pp: " + std::to_string(10.0 * std::log10(loud / soft)) + " dB");
}

} // namespace

// Each voicing control moves the sound the way its name says.
void voicingControls() {
    using pianocraft::ParameterId;
    // Returns the RMS of the first 0.5 s and of the following second (released at 0.5 s if asked).
    const auto play = [](ParameterId id, double value, int key, double velocity, bool release) {
        pianocraft::PianoEngine engine;
        engine.prepare(48000.0);
        engine.setParameter(id, value);
        engine.noteOn(note(key), velocity);
        const auto head = render(engine, 24000, 64);
        if (release) {
            engine.noteOff(note(key), 0.0);
        }
        return std::pair{head, render(engine, 48000, 64)};
    };
    const auto dB = [](double a, double b) { return 20.0 * std::log10(a / b); };
    const auto tail = [&](ParameterId id, double value, int key, bool release) {
        const auto after = play(id, value, key, 0.6, release).second;
        return rms(std::vector<float>(after.begin() + 24000, after.end()));  // 0.75-1.5 s
    };
    const auto check = [&](const char* name, double measured, double minimum) {
        require(measured > minimum, std::string(name) + ": " + std::to_string(measured) + " dB");
    };

    check("Sustain Length 3 vs 1, held tail", dB(tail(ParameterId::sustainLength, 3.0, 72, false),
                                                tail(ParameterId::sustainLength, 1.0, 72, false)), 3.0);
    check("Damper Release 4 vs 1, released tail", dB(tail(ParameterId::damperRelease, 4.0, 60, true),
                                                    tail(ParameterId::damperRelease, 1.0, 60, true)), 6.0);
    // The slow unison mode beats against the prompt sound early on; its own tail dominates after ~2 s.
    const auto lateTail = [](ParameterId id, double value) {
        pianocraft::PianoEngine engine;
        engine.prepare(48000.0);
        engine.setParameter(id, value);
        engine.noteOn(note(84), 0.6);
        render(engine, 96000, 64);
        return rms(render(engine, 48000, 64));
    };
    check("Aftersound 1 vs 0, treble tail at 2-3 s",
          dB(lateTail(ParameterId::aftersound, 1.0), lateTail(ParameterId::aftersound, 0.0)), 6.0);
    // Weinreich: perfectly tuned unisons struck level leave no aftersound.
    check("Unison Detune 1 vs 0, treble tail at 2-3 s",
          dB(lateTail(ParameterId::unisonDetune, 1.0), lateTail(ParameterId::unisonDetune, 0.0)), 6.0);

    const auto spread = [&](double dynamics) {
        return dB(rms(play(ParameterId::dynamicRange, dynamics, 60, 0.95, false).first),
                  rms(play(ParameterId::dynamicRange, dynamics, 60, 0.3, false).first));
    };
    const double normal = spread(1.0);
    check("Dynamic Range 2 widens ff-pp", spread(2.0) - 1.6 * normal, 0.0);
    check("Dynamic Range 0 narrows ff-pp", 0.4 * normal - spread(0.0), 0.0);

    const auto brightening = [&](double amount) {
        return 10.0 * std::log10(trebleShare(play(ParameterId::velocityBrightness, amount, 60, 0.95, false).first) /
                                 trebleShare(play(ParameterId::velocityBrightness, amount, 60, 0.3, false).first));
    };
    check("Velocity Brightness 2 vs 0, ff-pp brightening", brightening(2.0) - brightening(0.0), 6.0);

    // Detune and noise have no simple level signature; they must change the sound audibly.
    const auto changes = [&](ParameterId id, double low, double high, int key) {
        const auto a = play(id, low, key, 0.6, false).second;
        const auto b = play(id, high, key, 0.6, false).second;
        std::vector<float> difference(a.size());
        std::transform(a.begin(), a.end(), b.begin(), difference.begin(), std::minus<>());
        return dB(rms(a), rms(difference));
    };
    check("Unison Detune 3 vs 0 differs by more than -20 dB", 20.0 - changes(ParameterId::unisonDetune, 0.0, 3.0, 60), 0.0);
    const auto knock = [&](double noise) { return play(ParameterId::hammerNoise, noise, 60, 0.95, false).first; };
    std::vector<float> knockDifference(knock(0.0));
    const auto loud = knock(2.0);
    std::transform(knockDifference.begin(), knockDifference.end(), loud.begin(), knockDifference.begin(), std::minus<>());
    check("Hammer Noise 2 vs 0 differs by more than -30 dB", 30.0 - dB(rms(loud), rms(knockDifference)), 0.0);
}

// Half-pedalling damps between pedal-up and pedal-down, re-pedalling catches a decaying note, and
// the soft pedal makes notes softer and darker.
void pedalsAreContinuous() {
    const auto releasedTail = [](double depth, bool repedal) {
        pianocraft::PianoEngine engine;
        engine.prepare(48000.0);
        engine.setSustain(0, depth);
        engine.noteOn(note(48), 0.7);
        render(engine, 24000, 64);
        engine.noteOff(note(48), 0.0);
        render(engine, 2400, 64);
        if (repedal) {
            engine.setSustain(0, 1.0);
        }
        return rms(render(engine, 48000, 64));
    };
    const auto dB = [](double a, double b) { return 20.0 * std::log10(a / b); };
    const double down = releasedTail(1.0, false);
    const double half = releasedTail(0.5, false);
    const double up = releasedTail(0.0, false);
    require(dB(down, half) > 6.0 && dB(half, up) > 6.0,
            "Half pedal is not between down and up: " + std::to_string(dB(down, half)) + " / " +
            std::to_string(dB(half, up)) + " dB");
    require(dB(releasedTail(0.0, true), up) > 6.0, "Re-pedalling did not catch the decaying note");

    const auto play = [](double soft) {
        pianocraft::PianoEngine engine;
        engine.prepare(48000.0);
        engine.setSoftPedal(0, soft);
        engine.noteOn(note(60), 0.8);
        return render(engine, 19200, 64);
    };
    const auto normal = play(0.0);
    const auto soft = play(1.0);
    require(dB(rms(normal), rms(soft)) > 3.0, "Soft pedal is not softer: " + std::to_string(dB(rms(normal), rms(soft))) + " dB");
    require(trebleShare(soft) < trebleShare(normal) * 0.8, "Soft pedal is not darker");
}

// Master Tune sets A4 exactly; Stretch 0 is equal temperament and 2 doubles the recorded stretch.
void tuningControls() {
    pianocraft::InstrumentModel model(48000.0);
    require(std::abs(model.key(69).fundamental - 440.0) < 1.0e-9, "Default A4 is not 440 Hz");
    const double partial = model.key(69).modes[0].frequency;

    pianocraft::Voicing voicing;
    voicing.masterTune = 415.0;
    voicing.stretch = 0.0;
    model.setVoicing(voicing);
    for (int key = 21; key <= 108; ++key) {
        const double expected = 415.0 * std::exp2((key - 69) / 12.0);
        require(std::abs(model.key(key).fundamental / expected - 1.0) < 1.0e-9,
                "Stretch 0 is not equal temperament at key " + std::to_string(key));
    }
    require(std::abs(model.key(69).modes[0].frequency / partial - 415.0 / 440.0) < 1.0e-9,
            "Partials do not follow the tuning");

    const auto cents = [&](int key, double stretch) {
        voicing.masterTune = 440.0;
        voicing.stretch = stretch;
        model.setVoicing(voicing);
        return 1200.0 * std::log2(model.key(key).fundamental / (440.0 * std::exp2((key - 69) / 12.0)));
    };
    require(cents(108, 1.0) > 20.0 && std::abs(cents(108, 2.0) - 2.0 * cents(108, 1.0)) < 1.0e-6,
            "Stretch 2 does not double the recorded stretch");

    // The engine plays it: A4's period moves from 48000/440 to 48000/415 samples.
    const auto periodicity = [](double tune, int lag) {
        pianocraft::PianoEngine engine;
        engine.prepare(48000.0);
        engine.setParameter(pianocraft::ParameterId::masterTune, tune);
        engine.noteOn(note(69), 0.6);
        const auto output = render(engine, 24000, 64);
        double sum = 0.0;
        for (std::size_t index = 9600; index + 2 * lag < output.size(); index += 2) {
            sum += static_cast<double>(output[index]) * output[index + 2 * lag];
        }
        return sum;
    };
    require(periodicity(415.0, 116) > periodicity(415.0, 109) && periodicity(440.0, 109) > periodicity(440.0, 116),
            "Master Tune does not move the played pitch");
}

// Age wears each key differently but the same way every time; Age 0 is the calibrated piano.
void ageWearsUnevenly() {
    pianocraft::InstrumentModel fresh(48000.0);
    pianocraft::InstrumentModel worn(48000.0);
    pianocraft::Voicing voicing;
    voicing.age = 1.0;
    worn.setVoicing(voicing);
    const auto releaseSeconds = [](const pianocraft::ModeCoefficients& mode) {
        return -6.907755278982138 / (48000.0 * std::log(std::sqrt(mode.releaseA2)));
    };
    double sum = 0.0, squares = 0.0, softest = 10.0, hardest = 0.0, mostLeak = 0.0, leastLeak = 10.0;
    for (int midi = 21; midi <= 108; ++midi) {
        const auto& before = fresh.key(midi);
        const auto& after = worn.key(midi);
        const double cents = 1200.0 * std::log2(after.fundamental / before.fundamental);
        sum += cents;
        squares += cents * cents;
        softest = std::min(softest, after.wearHardness);
        hardest = std::max(hardest, after.wearHardness);
        if (before.hasDamper) {
            const double leak = releaseSeconds(after.modes[0]) / releaseSeconds(before.modes[0]);
            mostLeak = std::max(mostLeak, leak);
            leastLeak = std::min(leastLeak, leak);
        }
    }
    const double mean = sum / 88.0;
    const double spread = std::sqrt(squares / 88.0 - mean * mean);
    require(mean < -20.0 && mean > -30.0 && spread > 2.0,
            "Age 1 does not slacken pitch unevenly: mean " + std::to_string(mean) + " spread " + std::to_string(spread));
    require(hardest / softest > 1.4, "Age 1 does not vary hammer hardness from key to key");
    require(leastLeak >= 1.0 && mostLeak > 3.0, "Age 1 does not make some dampers leak");

    worn.setVoicing(pianocraft::Voicing{});
    for (int midi = 21; midi <= 108; ++midi) {
        require(worn.key(midi).fundamental == fresh.key(midi).fundamental &&
                    worn.key(midi).modes[0].releaseA2 == fresh.key(midi).modes[0].releaseA2 &&
                    worn.key(midi).wearGain == 1.0,
                "Age 0 is not the calibrated piano");
    }

    const auto play = [](double age) {
        pianocraft::PianoEngine engine;
        engine.setParameter(pianocraft::ParameterId::age, age);
        engine.prepare(48000.0);
        engine.noteOn(note(60), 0.6);
        return render(engine, 9600, 64);
    };
    pianocraft::PianoEngine plain;
    plain.prepare(48000.0);
    plain.noteOn(note(60), 0.6);
    require(play(0.0) == render(plain, 9600, 64), "Age 0 changes the default sound");
    require(play(0.7) == play(0.7) && play(0.7) != play(0.0), "Age is not a fixed, audible wear");
}

// Per-note modulation and expressions reach only their note, and ended voices are reported for
// CLAP NOTE_END, including a string restruck under a new note id.
void perNoteControl() {
    using pianocraft::ParameterId;
    using pianocraft::PianoEngine;
    const auto play = [](int targetId, auto setup) {
        PianoEngine engine;
        engine.prepare(48000.0);
        engine.noteOn(note(60, 1), 0.6);
        setup(engine, pianocraft::NoteAddress{targetId, -1, -1, -1});
        return render(engine, 19200, 64);
    };
    const auto none = [](PianoEngine&, pianocraft::NoteAddress) {};
    const auto harder = [](PianoEngine& engine, pianocraft::NoteAddress target) {
        engine.modulate(target, ParameterId::hammerHardness, 1.0);
    };
    const auto plain = play(1, none);
    const auto modulated = play(1, harder);
    require(play(2, harder) == plain, "Modulating another note changed this one");
    require(play(-1, harder) == modulated, "Global modulation differs from modulating every note");
    require(trebleShare(modulated) > trebleShare(plain) * 1.2, "Per-note hardness had no effect");
    const auto timbre = [](double value) {
        return [value](PianoEngine& engine, pianocraft::NoteAddress target) { engine.setNoteBrightness(target, value); };
    };
    require(play(1, timbre(0.5)) == plain, "Neutral Timbre changed the note");
    require(play(1, timbre(1.0)) == modulated, "Full Timbre is not Hammer Hardness +1");
    require(play(2, timbre(1.0)) == plain, "Timbre on another note changed this one");
    require(trebleShare(play(1, timbre(0.0))) < trebleShare(plain), "Low Timbre did not soften the hammer");

    const auto quieter = play(1, [](PianoEngine& engine, pianocraft::NoteAddress target) {
        engine.setNoteVolume(target, 0.5);
    });
    require(std::abs(20.0 * std::log10(rms(quieter) / rms(plain)) + 6.02) < 0.1, "Volume expression is not -6 dB at 0.5");
    const auto rightShare = [](const std::vector<float>& output) {
        double left = 0.0;
        double right = 0.0;
        for (std::size_t index = 0; index < output.size(); index += 2) {
            left += static_cast<double>(output[index]) * output[index];
            right += static_cast<double>(output[index + 1]) * output[index + 1];
        }
        return right / (left + right);
    };
    const auto panned = play(1, [](PianoEngine& engine, pianocraft::NoteAddress target) {
        engine.setNotePan(target, 0.0);
    });
    require(rightShare(panned) < rightShare(plain) * 0.5, "Pan expression did not move the note left");

    PianoEngine engine;
    engine.prepare(48000.0);
    engine.noteOn(note(60, 1), 0.6);
    render(engine, 4800, 64);
    engine.noteOff(note(60, 1), 0.0);
    engine.noteOn(note(60, 3), 0.6);
    engine.choke(note(60, 3));
    render(engine, 24000, 24000);
    std::array<PianoEngine::EndedNote, 4> ended{};
    const auto count = engine.takeEndedNotes(ended.data(), ended.size());
    require(count == 2 && ended[0].address.noteId == 1 && ended[0].frame == 0 &&
            ended[1].address.noteId == 3 && ended[1].frame > 0 && engine.activeVoiceCount() == 0,
            "Ended notes not reported: count " + std::to_string(count));
    require(engine.takeEndedNotes(ended.data(), ended.size()) == 0, "Ended notes were not cleared");
}

int main() {
    try {
        modelIsStable();
        noteProducesSoundAndReleases();
        sustainHoldsNotes();
        restrikesAndPolyphonyAreFinite();
        nothingClicks();
        pedalMakesNoise();
        blockSizesAreDeterministic();
        parametersAndRatesAreValid();
        sympatheticIsSelective();
        bodyAndSympatheticAffectOutput();
        trebleIsBalancedAndClear();
        notesSustainInStereo();
        micPositionsAndWidth();
        louderIsBrighter();
        voicingControls();
        pedalsAreContinuous();
        tuningControls();
        ageWearsUnevenly();
        perNoteControl();
        std::cout << "All engine tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
