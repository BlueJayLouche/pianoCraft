#include "engine/PianoEngine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
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
    const std::uint32_t dataSize = static_cast<std::uint32_t>(samples.size() * 3);
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
    if (!stream) {
        throw std::runtime_error("Failed while writing output WAV");
    }
}

struct Event {
    enum class Type { noteOn, noteOff, pedal } type;
    double time;
    int key;
    double velocity;
};

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cerr << "Usage: pianocraft-render <output.wav> [midi] [velocity] [seconds] [pedal] [body] [sympathy] [mic] [id=value ...]\n";
            return 2;
        }
        const std::string output = argv[1];
        const int key = argc > 2 ? std::stoi(argv[2]) : 60;
        const double velocity = argc > 3 ? std::stod(argv[3]) : 0.8;
        const double seconds = argc > 4 ? std::stod(argv[4]) : 8.0;
        const int pedal = argc > 5 ? std::stoi(argv[5]) : 0;
        const double body = argc > 6 ? std::stod(argv[6]) : 1.0;
        const double sympathy = argc > 7 ? std::stod(argv[7]) : 0.35;
        const int mic = argc > 8 ? std::stoi(argv[8]) : 0;
        if (key < 21 || key > 108 || velocity < 0.0 || velocity > 1.0 || seconds <= 0.0 || seconds > 120.0 ||
            (pedal != 0 && pedal != 1) || body < 0.0 || body > 2.0 || sympathy < 0.0 || sympathy > 1.0 ||
            mic < 0 || mic > 2) {
            throw std::runtime_error("Invalid key, velocity, duration, pedal, body, sympathy, or mic value");
        }

        constexpr std::uint32_t sampleRate = 48000;
        constexpr std::uint32_t blockSize = 64;
        pianocraft::PianoEngine engine;
        engine.prepare(sampleRate);
        engine.setParameter(pianocraft::ParameterId::bodyLevel, body);
        engine.setParameter(pianocraft::ParameterId::sympatheticLevel, sympathy);
        engine.setParameter(pianocraft::ParameterId::micPosition, mic);
        for (int index = 9; index < argc; ++index) { // any parameter by its id, e.g. 13=0 for no hammer noise
            const std::string setting = argv[index];
            const auto equals = setting.find('=');
            if (equals == std::string::npos) {
                throw std::runtime_error("Expected id=value: " + setting);
            }
            engine.setParameter(static_cast<pianocraft::ParameterId>(std::stoi(setting.substr(0, equals))),
                                std::stod(setting.substr(equals + 1)));
        }
        const double noteOffTime = std::min(1.5, seconds * 0.4);
        std::vector<Event> events;
        if (pedal == 1) {
            events.push_back({Event::Type::pedal, 0.0, key, 1.0});
        }
        events.push_back({Event::Type::noteOn, 0.05, key, velocity});
        events.push_back({Event::Type::noteOff, noteOffTime, key, 0.0});
        if (pedal == 1) {
            events.push_back({Event::Type::pedal, std::max(noteOffTime, seconds * 0.85), key, 0.0});
        }
        std::size_t nextEvent = 0;
        std::uint64_t renderedFrames = 0;
        const auto totalFrames = static_cast<std::uint64_t>(seconds * sampleRate);
        std::vector<float> samples;
        samples.reserve(static_cast<std::size_t>(totalFrames * 2));
        std::array<float, blockSize> left{};
        std::array<float, blockSize> right{};
        auto append = [&](std::uint32_t count) {
            for (std::uint32_t index = 0; index < count; ++index) {
                samples.push_back(left[index]);
                samples.push_back(right[index]);
            }
        };

        while (renderedFrames < totalFrames) {
            const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(blockSize, totalFrames - renderedFrames));
            std::uint32_t offset = 0;
            while (nextEvent < events.size()) {
                const auto eventFrame = static_cast<std::uint64_t>(events[nextEvent].time * sampleRate);
                if (eventFrame >= renderedFrames + frames) {
                    break;
                }
                const auto local = static_cast<std::uint32_t>(std::max<std::uint64_t>(eventFrame, renderedFrames) - renderedFrames);
                if (local > offset) {
                    engine.render(left.data(), right.data(), local);
                    append(local);
                    offset = local;
                }
                const auto& event = events[nextEvent];
                const pianocraft::NoteAddress address{-1, 0, 0, static_cast<std::int16_t>(event.key)};
                switch (event.type) {
                    case Event::Type::noteOn:
                        engine.noteOn(address, event.velocity);
                        break;
                    case Event::Type::noteOff:
                        engine.noteOff(address, 0.0);
                        break;
                    case Event::Type::pedal:
                        engine.setSustain(0, event.velocity);
                        break;
                }
                ++nextEvent;
            }
            if (offset < frames) {
                const auto remaining = frames - offset;
                engine.render(left.data(), right.data(), remaining);
                append(remaining);
            }
            renderedFrames += frames;
        }

        writeWav(output, samples, sampleRate);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
