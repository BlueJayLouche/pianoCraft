#include "engine/PianoEngine.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

int main() {
    constexpr std::uint32_t sampleRate = 48000;
    constexpr std::uint32_t blockSize = 64;
    constexpr std::uint32_t blockCount = 3750;
    pianocraft::PianoEngine engine;
    engine.prepare(sampleRate);
    engine.setSustain(0, true);
    for (int key = 36; key < 100; ++key) {
        engine.noteOn({key, 0, 0, static_cast<std::int16_t>(key)}, 0.8);
    }

    std::array<float, blockSize> left{};
    std::array<float, blockSize> right{};
    std::vector<double> callbacks;
    callbacks.reserve(blockCount);
    volatile double checksum = 0.0;
    for (std::uint32_t block = 0; block < blockCount; ++block) {
        const auto start = std::chrono::steady_clock::now();
        engine.render(left.data(), right.data(), blockSize);
        const auto end = std::chrono::steady_clock::now();
        callbacks.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        checksum += left[blockSize / 2] + right[blockSize / 2];
    }
    std::sort(callbacks.begin(), callbacks.end());
    const auto percentile = [&](double value) {
        return callbacks[std::min(callbacks.size() - 1,
            static_cast<std::size_t>(value * (callbacks.size() - 1)))];
    };
    const double deadline = 1.0e6 * blockSize / sampleRate;
    std::cout << std::fixed << std::setprecision(2)
              << "callbacks=" << callbacks.size() << " block_size=" << blockSize
              << " deadline_us=" << deadline << '\n'
              << "p50_us=" << percentile(0.5) << " p99_us=" << percentile(0.99)
              << " p99.9_us=" << percentile(0.999) << " max_us=" << callbacks.back() << '\n'
              << "p99.9_deadline_fraction=" << percentile(0.999) / deadline
              << " max_deadline_fraction=" << callbacks.back() / deadline << '\n'
              << "checksum=" << checksum << '\n';
    return percentile(0.999) < deadline * 0.5 && callbacks.back() < deadline * 0.8 ? 0 : 1;
}
