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
            require(value.velocityBrightnessExponent >= 0.0 && value.velocityBrightnessExponent <= 2.5,
                    "Invalid velocity brightness exponent");
            require(value.hammerStiffnessScale >= 0.5 && value.hammerStiffnessScale <= 2.0,
                    "Invalid hammer stiffness scale");
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
    // 12 ringing restrikes plus 64 new keys fit in the 96 voices without stealing.
    require(engine.activeVoiceCount() == 76, "Expected 76 active voices");
    const auto output = render(engine, 2400, 64);
    require(std::isfinite(peak(output)), "Dense polyphony failed");
    engine.reset();
    require(engine.activeVoiceCount() == 0, "Reset left voices active");
    require(peak(render(engine, 256, 64)) == 0.0, "Reset did not silence output");
}

void blockSizesAreDeterministic() {
    pianocraft::PianoEngine first;
    pianocraft::PianoEngine second;
    first.prepare(48000.0);
    second.prepare(48000.0);
    first.noteOn(note(60), 0.75);
    second.noteOn(note(60), 0.75);
    const auto a = render(first, 4096, 64);
    const auto b = render(second, 4096, 127);
    require(a == b, "Output depends on host block size");
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
    // The partner beats against the prompt sound early on; its own tail dominates after ~2 s.
    const auto lateTail = [](double aftersound) {
        pianocraft::PianoEngine engine;
        engine.prepare(48000.0);
        engine.setParameter(ParameterId::aftersound, aftersound);
        engine.noteOn(note(84), 0.6);
        render(engine, 96000, 64);
        return rms(render(engine, 48000, 64));
    };
    check("Aftersound 1 vs 0, treble tail at 2-3 s", dB(lateTail(1.0), lateTail(0.0)), 6.0);

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

int main() {
    try {
        modelIsStable();
        noteProducesSoundAndReleases();
        sustainHoldsNotes();
        restrikesAndPolyphonyAreFinite();
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
        std::cout << "All engine tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
