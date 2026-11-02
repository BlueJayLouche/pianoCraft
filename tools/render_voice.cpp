// Renders one harp, flute, strings, horn, trumpet, trombone, tuba, oboe or clarinet note to a 24-bit stereo WAV at 48 kHz:
//   pianocraft-render-voice <harp|flute|strings|horn|trumpet|trombone|tuba|oboe|clarinet> out.wav key velocity seconds [hold] [id=value ...]
// hold is when the key is released (default: 40% of the length); id=value sets a parameter by id,
// ccN=value sends MIDI controller N (0-1) before the note; slurK=seconds presses key K then, the
// sounding note released 10 ms later (a slur), and the last note is released at hold.
#include "engine/FluteEngine.hpp"
#include "engine/HarpEngine.hpp"
#include "engine/BrassEngine.hpp"
#include "engine/ReedEngine.hpp"
#include "engine/StringsEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void write16(std::ostream& stream, std::uint16_t value) {
    stream.put(static_cast<char>(value & 0xff));
    stream.put(static_cast<char>((value >> 8) & 0xff));
}

void write32(std::ostream& stream, std::uint32_t value) {
    write16(stream, value & 0xffff);
    write16(stream, (value >> 16) & 0xffff);
}

void writeWav(const std::string& path, const std::vector<float>& samples, std::uint32_t sampleRate) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open output WAV");
    }
    const auto dataSize = static_cast<std::uint32_t>(samples.size() * 3);
    stream.write("RIFF", 4);
    write32(stream, 36 + dataSize);
    stream.write("WAVEfmt ", 8);
    write32(stream, 16);
    write16(stream, 1);
    write16(stream, 2);
    write32(stream, sampleRate);
    write32(stream, sampleRate * 6);
    write16(stream, 6);
    write16(stream, 24);
    stream.write("data", 4);
    write32(stream, dataSize);
    for (float sample : samples) {
        const auto value = static_cast<std::int32_t>(std::lround(std::clamp(sample, -1.0f, 1.0f) * 8388607.0f));
        stream.put(static_cast<char>(value & 0xff));
        stream.put(static_cast<char>((value >> 8) & 0xff));
        stream.put(static_cast<char>((value >> 16) & 0xff));
    }
}

template <class Engine, class Id>
std::vector<float> renderNote(int argc, char** argv, int key, double velocity, double seconds, double hold) {
    constexpr std::uint32_t sampleRate = 48000;
    constexpr std::uint32_t block = 64;
    Engine engine;
    engine.prepare(sampleRate);
    std::vector<std::pair<std::uint64_t, int>> slurs;
    for (int i = 7; i < argc; ++i) {
        const std::string assignment = argv[i];
        const auto equals = assignment.find('=');
        if (equals == std::string::npos) {
            throw std::runtime_error("Expected id=value: " + assignment);
        }
        const double value = std::stod(assignment.substr(equals + 1));
        if (assignment.rfind("slur", 0) == 0) {
            slurs.emplace_back(static_cast<std::uint64_t>(value * sampleRate), std::stoi(assignment.substr(4, equals - 4)));
        } else if (assignment.rfind("cc", 0) == 0) {
            engine.controlChange(0, static_cast<std::uint8_t>(std::stoi(assignment.substr(2, equals - 2))), value);
        } else {
            engine.setParameter(static_cast<Id>(std::stoi(assignment.substr(0, equals))), value);
        }
    }
    pianocraft::NoteAddress note{-1, 0, 0, static_cast<std::int16_t>(key)};
    pianocraft::NoteAddress previous = note;
    std::uint64_t previousOff = 0;
    const auto total = static_cast<std::uint64_t>(seconds * sampleRate);
    const auto on = static_cast<std::uint64_t>(0.05 * sampleRate);
    const auto off = static_cast<std::uint64_t>(hold * sampleRate);
    std::vector<float> samples;
    std::vector<float> left(block), right(block);
    for (std::uint64_t frame = 0; frame < total; frame += block) {
        if (frame <= on && on < frame + block) {
            engine.noteOn(note, velocity);
        }
        for (const auto& [at, slurKey] : slurs) {
            if (frame <= at && at < frame + block) {
                previous = note;
                previousOff = at + sampleRate / 100;
                note = {-1, 0, 0, static_cast<std::int16_t>(slurKey)};
                engine.noteOn(note, velocity);
            }
        }
        if (previousOff != 0 && frame <= previousOff && previousOff < frame + block) {
            engine.noteOff(previous, 0.0);
            previousOff = 0;
        }
        if (frame <= off && off < frame + block) {
            engine.noteOff(note, 0.0);
        }
        const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(block, total - frame));
        engine.render(left.data(), right.data(), count);
        for (std::uint32_t i = 0; i < count; ++i) {
            samples.push_back(left[i]);
            samples.push_back(right[i]);
        }
    }
    return samples;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 6) {
            std::cerr << "Usage: pianocraft-render-voice <harp|flute|strings|horn|trumpet|trombone|tuba|oboe|clarinet> out.wav key velocity seconds [hold] [id=value ...]\n";
            return 2;
        }
        const std::string instrument = argv[1];
        const int key = std::stoi(argv[3]);
        const double velocity = std::stod(argv[4]);
        const double seconds = std::stod(argv[5]);
        const double hold = argc > 6 ? std::stod(argv[6]) : seconds * 0.4;
        if (velocity < 0.0 || velocity > 1.0 || seconds <= 0.0 || seconds > 120.0) {
            throw std::runtime_error("Invalid velocity or duration");
        }
        const auto samples = instrument == "harp"
            ? renderNote<pianocraft::HarpEngine, pianocraft::HarpParameterId>(argc, argv, key, velocity, seconds, hold)
            : instrument == "flute"
            ? renderNote<pianocraft::FluteEngine, pianocraft::FluteParameterId>(argc, argv, key, velocity, seconds, hold)
            : instrument == "strings"
            ? renderNote<pianocraft::StringsEngine, pianocraft::StringsParameterId>(argc, argv, key, velocity, seconds, hold)
            : instrument == "horn"
            ? renderNote<pianocraft::HornEngine, pianocraft::HornParameterId>(argc, argv, key, velocity, seconds, hold)
            : instrument == "trumpet"
            ? renderNote<pianocraft::TrumpetEngine, pianocraft::BrassParameterId>(argc, argv, key, velocity, seconds, hold)
            : instrument == "trombone"
            ? renderNote<pianocraft::TromboneEngine, pianocraft::BrassParameterId>(argc, argv, key, velocity, seconds, hold)
            : instrument == "tuba"
            ? renderNote<pianocraft::TubaEngine, pianocraft::BrassParameterId>(argc, argv, key, velocity, seconds, hold)
            : instrument == "oboe"
            ? renderNote<pianocraft::OboeEngine, pianocraft::ReedParameterId>(argc, argv, key, velocity, seconds, hold)
            : instrument == "clarinet"
            ? renderNote<pianocraft::ClarinetEngine, pianocraft::ReedParameterId>(argc, argv, key, velocity, seconds, hold)
            : throw std::runtime_error("Instrument must be harp, flute, strings, horn, trumpet, trombone, tuba, oboe or clarinet");
        writeWav(argv[2], samples, 48000);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
