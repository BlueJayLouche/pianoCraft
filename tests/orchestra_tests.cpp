// The final act's promises in docs/old-house-needs.md: in tune, harp strings that keep ringing
// through a glissando, harmonics an octave up, mono-safe width; a flute whose held notes hold, whose
// overlapping notes slur, and whose CC1 is breath; strings and horns that sustain, whose CC1 is
// bow or breath, with their articulations. Every render must repeat exactly.
#include "engine/FluteEngine.hpp"
#include "engine/HarpEngine.hpp"
#include "engine/HornEngine.hpp"
#include "engine/StringsEngine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

using pianocraft::FluteEngine;
using pianocraft::FluteParameterId;
using pianocraft::HarpEngine;
using pianocraft::HarpParameterId;
using pianocraft::HornEngine;
using pianocraft::HornParameterId;
using pianocraft::StringsEngine;
using pianocraft::StringsParameterId;
using pianocraft::NoteAddress;

constexpr double pi = 3.141592653589793;
int failures = 0;

void expect(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    failures += !ok;
}

NoteAddress note(int key, int id = -1) {
    return NoteAddress{id, 0, 0, static_cast<std::int16_t>(key)};
}

// Renders in 64-frame blocks; events[i] runs before block i.
template <class Engine>
std::vector<float> render(Engine& engine, double seconds, double rate,
                          const std::function<void(std::size_t frame)>& events = {}, std::vector<float>* right = nullptr) {
    std::vector<float> left;
    std::vector<float> l(64), r(64);
    for (std::size_t frame = 0; frame < static_cast<std::size_t>(seconds * rate); frame += 64) {
        if (events) {
            events(frame);
        }
        engine.render(l.data(), r.data(), 64);
        left.insert(left.end(), l.begin(), l.end());
        if (right != nullptr) {
            right->insert(right->end(), r.begin(), r.end());
        }
    }
    return left;
}

double rmsDb(const std::vector<float>& x, double from, double to, double rate) {
    double sum = 0.0;
    const auto a = static_cast<std::size_t>(from * rate), b = std::min(x.size(), static_cast<std::size_t>(to * rate));
    for (std::size_t i = a; i < b; ++i) {
        sum += static_cast<double>(x[i]) * x[i];
    }
    return 10.0 * std::log10(sum / static_cast<double>(std::max<std::size_t>(b - a, 1)) + 1e-30);
}

double powerAt(const std::vector<float>& x, double from, double to, double rate, double frequency) {
    double re = 0.0, im = 0.0;
    const auto a = static_cast<std::size_t>(from * rate), b = std::min(x.size(), static_cast<std::size_t>(to * rate));
    for (std::size_t i = a; i < b; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(i - a) / static_cast<double>(b - a));
        re += x[i] * w * std::cos(2.0 * pi * frequency * static_cast<double>(i) / rate);
        im += x[i] * w * std::sin(2.0 * pi * frequency * static_cast<double>(i) / rate);
    }
    return re * re + im * im;
}

// The strongest frequency within +-60 cents of the nominal, in cents from it.
double centsOff(const std::vector<float>& x, double from, double to, double rate, double nominal) {
    double best = 0.0, bestPower = -1.0;
    for (double cents = -60.0; cents <= 60.0; cents += 0.5) {
        const double power = powerAt(x, from, to, rate, nominal * std::pow(2.0, cents / 1200.0));
        if (power > bestPower) {
            bestPower = power;
            best = cents;
        }
    }
    return best;
}

double frequency(int key) {
    return 440.0 * std::pow(2.0, (key - 69) / 12.0);
}

// The level above 3 kHz (four one-pole high-passes) over the whole level, in dB. Higher is brighter.
double tilt(const std::vector<float>& x, double from, double to, double rate) {
    std::vector<float> y = x;
    const double a = std::exp(-2.0 * pi * 3000.0 / rate);
    for (int pass = 0; pass < 4; ++pass) {
        double previousIn = 0.0, previousOut = 0.0;
        for (auto& v : y) {
            const double out = a * (previousOut + v - previousIn);
            previousIn = v;
            v = static_cast<float>(previousOut = out);
        }
    }
    return rmsDb(y, from, to, rate) - rmsDb(x, from, to, rate);
}

// Peak-to-trough of the 20 ms level, in dB (95th over 5th percentile).
double flutter(const std::vector<float>& x, double from, double to, double rate) {
    std::vector<double> levels;
    for (double t = from; t + 0.02 <= to; t += 0.01) {
        levels.push_back(rmsDb(x, t, t + 0.02, rate));
    }
    std::sort(levels.begin(), levels.end());
    return levels[levels.size() * 95 / 100] - levels[levels.size() * 5 / 100];
}

// Sustained voices: in tune, held notes hold, CC1 louder and brighter, mono at width 0, a released
// note ends, and every render repeats.
template <class Engine, class Id>
std::vector<float> sustain(int key, double breath, double seconds, double rate, std::vector<std::pair<Id, double>> settings,
                           std::vector<float>* right = nullptr, std::size_t* ended = nullptr, double releaseAt = 1e9) {
    Engine engine;
    engine.prepare(rate);
    for (const auto& [id, value] : settings) {
        engine.setParameter(id, value);
    }
    engine.controlChange(0, 1, breath);
    const auto release = static_cast<std::size_t>(releaseAt * rate) / 64 * 64;
    auto x = render(engine, seconds, rate, [&](std::size_t f) {
        if (f == 0) engine.noteOn(note(key, 9), 0.7);
        if (f == release) engine.noteOff(note(key, 9), 0.0);
        if (ended != nullptr) {
            std::array<pianocraft::EndedNote, 8> notes{};
            *ended += engine.takeEndedNotes(notes.data(), notes.size());
        }
    }, right);
    if (ended != nullptr && engine.activeVoiceCount() != 0) {
        *ended = 99;
    }
    return x;
}

void harpTests() {
    for (const double rate : {48000.0, 96000.0}) {
        HarpEngine harp;
        harp.prepare(rate);
        const auto x = render(harp, 1.2, rate, [&](std::size_t f) { if (f == 0) harp.noteOn(note(60), 0.8); });
        const double off = centsOff(x, 0.2, 1.2, rate, frequency(60));
        expect(std::abs(off) <= 1.0, "harp C4 in tune at " + std::to_string(static_cast<int>(rate)) + " Hz: " + std::to_string(off) + " cents");
    }
    {
        HarpEngine harp;
        harp.prepare(48000.0);
        const auto x = render(harp, 3.0, 48000.0, [&](std::size_t f) {
            if (f == 0) harp.noteOn(note(60), 0.8);
            if (f == 4800) harp.noteOff(note(60), 0.0);
        });
        expect(rmsDb(x, 2.5, 3.0, 48000.0) - rmsDb(x, 0.1, 0.6, 48000.0) > -25.0, "harp strings ring after note-off");
        HarpEngine damped;
        damped.prepare(48000.0);
        damped.setParameter(HarpParameterId::damping, 1.0);
        const auto y = render(damped, 2.0, 48000.0, [&](std::size_t f) {
            if (f == 0) damped.noteOn(note(60), 0.8);
            if (f == 4800) damped.noteOff(note(60), 0.0);
        });
        expect(rmsDb(y, 1.5, 2.0, 48000.0) - rmsDb(y, 0.0, 0.1, 48000.0) < -60.0, "Note-Off Damping 1 stops the string");
    }
    {
        // A glissando up two octaves of white keys, each released 20 ms after its pluck.
        HarpEngine harp;
        harp.prepare(48000.0);
        const int keys[] = {48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72};
        render(harp, 1.0, 48000.0, [&](std::size_t f) {
            for (std::size_t i = 0; i < std::size(keys); ++i) {
                const std::size_t at = i * 1536; // 32 ms apart
                if (f == at) harp.noteOn(note(keys[i]), 0.7);
                if (f == at + 960) harp.noteOff(note(keys[i]), 0.0);
            }
        });
        expect(harp.activeVoiceCount() == std::size(keys), "every string of a glissando keeps ringing");
    }
    {
        HarpEngine harp;
        harp.prepare(48000.0);
        harp.setParameter(HarpParameterId::harmonics, 1.0);
        const auto x = render(harp, 1.5, 48000.0, [&](std::size_t f) { if (f == 0) harp.noteOn(note(60), 0.8); });
        const double sounding = powerAt(x, 0.3, 1.5, 48000.0, frequency(60));
        const double stringFundamental = powerAt(x, 0.3, 1.5, 48000.0, frequency(48));
        expect(10.0 * std::log10(stringFundamental / sounding) < -40.0, "harmonics sound the key, not the string an octave down");
    }
    {
        HarpEngine harp;
        harp.prepare(48000.0);
        harp.setParameter(HarpParameterId::stereoWidth, 0.0);
        std::vector<float> right;
        const auto left = render(harp, 0.5, 48000.0, [&](std::size_t f) {
            if (f == 0) { harp.noteOn(note(40), 0.8); harp.noteOn(note(84), 0.8); }
        }, &right);
        expect(left == right, "harp at Stereo Width 0 is exactly mono");
    }
    {
        const auto take = [] {
            HarpEngine harp;
            harp.prepare(48000.0);
            return render(harp, 0.5, 48000.0, [&](std::size_t f) { if (f == 0) harp.noteOn(note(55), 0.6); });
        };
        expect(take() == take(), "harp renders repeat exactly");
    }
}

void fluteTests() {
    for (const double rate : {48000.0, 96000.0}) {
        for (const int key : {61, 69, 76, 84, 96}) {
            FluteEngine flute;
            flute.prepare(rate);
            flute.controlChange(0, 1, 0.6);
            flute.setParameter(FluteParameterId::vibratoDepth, 0.0);
            const auto x = render(flute, 2.0, rate, [&](std::size_t f) { if (f == 0) flute.noteOn(note(key), 0.7); });
            const double off = centsOff(x, 1.0, 2.0, rate, frequency(key));
            expect(std::abs(off) <= 5.0, "flute key " + std::to_string(key) + " in tune at " + std::to_string(static_cast<int>(rate)) +
                                             " Hz: " + std::to_string(off) + " cents");
        }
    }
    {
        FluteEngine flute;
        flute.prepare(48000.0);
        const auto x = render(flute, 7.0, 48000.0, [&](std::size_t f) { if (f == 0) flute.noteOn(note(64), 0.7); });
        expect(std::abs(rmsDb(x, 6.0, 7.0, 48000.0) - rmsDb(x, 1.0, 2.0, 48000.0)) < 1.0, "a held flute note holds its level");
    }
    {
        // An approach note slurs into the melody note a third of a beat later and is released after it.
        FluteEngine flute;
        flute.prepare(48000.0);
        flute.setParameter(FluteParameterId::vibratoDepth, 0.0);
        std::size_t endedBeforeSlur = 0, endedAtSlur = 0;
        const auto x = render(flute, 2.0, 48000.0, [&](std::size_t f) {
            if (f == 0) flute.noteOn(note(62, 1), 0.7);
            if (f == 24000) {
                std::array<pianocraft::EndedNote, 4> ended{};
                endedBeforeSlur = flute.takeEndedNotes(ended.data(), ended.size());
                flute.noteOn(note(64, 2), 0.7);
                endedAtSlur = flute.takeEndedNotes(ended.data(), ended.size());
                expect(endedAtSlur == 1 && ended[0].address.noteId == 1, "the slurred-from note is reported ended");
            }
            if (f == 26880) flute.noteOff(note(62, 1), 0.0);
        });
        double quietest = 0.0;
        for (double t = 0.45; t < 0.7; t += 0.005) {
            quietest = std::min(quietest, rmsDb(x, t, t + 0.01, 48000.0) - rmsDb(x, 0.3, 0.45, 48000.0));
        }
        expect(endedBeforeSlur == 0 && quietest > -6.0, "a slur doesn't re-attack or gap (dip " + std::to_string(quietest) + " dB)");
        expect(std::abs(centsOff(x, 1.0, 2.0, 48000.0, frequency(64))) <= 5.0 && flute.activeVoiceCount() == 1,
               "after the slur the melody note sounds, and releasing the approach note doesn't stop it");
    }
    {
        // Fletcher (JASA 57, 1975): from p to f the low octave's upper partials grow and its
        // fundamental barely moves; in the third octave the fundamental itself grows.
        const auto take = [](int key, double breath) {
            FluteEngine flute;
            flute.prepare(48000.0);
            flute.setParameter(FluteParameterId::vibratoDepth, 0.0);
            flute.controlChange(0, 1, breath);
            return render(flute, 2.0, 48000.0, [&](std::size_t f) { if (f == 0) flute.noteOn(note(key), 0.7); });
        };
        const auto rise = [](const std::vector<float>& soft, const std::vector<float>& loud, double frequency) {
            return 10.0 * std::log10(powerAt(loud, 1.0, 2.0, 48000.0, frequency) / powerAt(soft, 1.0, 2.0, 48000.0, frequency));
        };
        const auto soft = take(69, 0.3), loud = take(69, 1.0);
        const double h1 = rise(soft, loud, frequency(69)), h3 = rise(soft, loud, 3.0 * frequency(69));
        expect(rmsDb(loud, 1.0, 2.0, 48000.0) - rmsDb(soft, 1.0, 2.0, 48000.0) > 4.0 && h1 < 6.0 && h3 - h1 > 8.0,
               "CC1 is breath: at A4 the upper partials grow (H1 +" + std::to_string(h1) + ", H3 +" + std::to_string(h3) + " dB)");
        // C7 is nearly pure, so its level is its fundamental's (and breath bends it a few cents,
        // which a single-frequency probe would mistake for level).
        const double top = rmsDb(take(96, 1.0), 1.0, 2.0, 48000.0) - rmsDb(take(96, 0.3), 1.0, 2.0, 48000.0);
        expect(top > 8.0, "in the third octave breath moves the fundamental (C7 +" + std::to_string(top) + " dB)");
    }
    {
        FluteEngine flute;
        flute.prepare(48000.0);
        flute.setParameter(FluteParameterId::breathNoise, 0.0);
        const auto x = render(flute, 1.0, 48000.0, [&](std::size_t f) { if (f == 0) flute.noteOn(note(69), 0.7); });
        expect(rmsDb(x, 0.5, 1.0, 48000.0) > -30.0, "the flute speaks with Breath Noise at 0");
    }
    {
        FluteEngine flute;
        flute.prepare(48000.0);
        std::size_t ended = 0;
        render(flute, 2.0, 48000.0, [&](std::size_t f) {
            if (f == 0) flute.noteOn(note(69, 3), 0.7);
            if (f == 48000) flute.noteOff(note(69, 3), 0.0);
            std::array<pianocraft::EndedNote, 4> notes{};
            ended += flute.takeEndedNotes(notes.data(), notes.size());
        });
        expect(ended == 1 && flute.activeVoiceCount() == 0, "a released flute note ends within a second");
    }
    {
        const auto take = [] {
            FluteEngine flute;
            flute.prepare(48000.0);
            return render(flute, 1.0, 48000.0, [&](std::size_t f) { if (f == 0) flute.noteOn(note(72), 0.6); });
        };
        expect(take() == take(), "flute renders repeat exactly");
    }
    {
        // piano.py's slides touch their notes: released and pressed at the same instant, they slur;
        // a repeated pitch is tongued.
        const auto touching = [](int next) {
            FluteEngine flute;
            flute.prepare(48000.0);
            flute.setParameter(FluteParameterId::vibratoDepth, 0.0);
            std::size_t endedAtSlur = 0;
            const auto x = render(flute, 1.0, 48000.0, [&](std::size_t f) {
                if (f == 0) flute.noteOn(note(62, 1), 0.7);
                if (f == 24000) {
                    flute.noteOff(note(62, 1), 0.0);
                    flute.noteOn(note(next, 2), 0.7);
                }
            });
            double quietest = 0.0;
            for (double t = 0.5; t < 0.6; t += 0.005) {
                quietest = std::min(quietest, rmsDb(x, t, t + 0.01, 48000.0) - rmsDb(x, 0.35, 0.5, 48000.0));
            }
            return quietest;
        };
        expect(touching(64) > -6.0 && touching(62) < -6.0, "touching notes slur (dip " + std::to_string(touching(64)) +
                                                              " dB); a repeated one is tongued (" + std::to_string(touching(62)) + " dB)");
    }
}

void stringsTests() {
    using S = StringsParameterId;
    const auto solo = std::vector<std::pair<S, double>>{{S::section, 1.0}, {S::vibratoDepth, 0.0}};
    for (const double rate : {48000.0, 96000.0}) {
        for (const int key : {36, 55, 69, 84}) {
            const auto x = sustain<StringsEngine, S>(key, 0.6, 2.0, rate, solo);
            const double off = centsOff(x, 1.0, 2.0, rate, frequency(key));
            expect(std::abs(off) <= 5.0, "string key " + std::to_string(key) + " in tune at " + std::to_string(static_cast<int>(rate)) +
                                             " Hz: " + std::to_string(off) + " cents");
        }
    }
    {
        auto harmonics = solo;
        harmonics.push_back({S::harmonics, 1.0});
        const auto x = sustain<StringsEngine, S>(69, 0.6, 2.0, 48000.0, harmonics);
        expect(std::abs(centsOff(x, 1.0, 2.0, 48000.0, frequency(69))) <= 5.0, "string harmonics sound the key, in tune");
    }
    {
        const auto x = sustain<StringsEngine, S>(64, 0.6, 7.0, 48000.0, solo);
        const double drift = rmsDb(x, 6.0, 7.0, 48000.0) - rmsDb(x, 1.0, 2.0, 48000.0);
        expect(std::abs(drift) < 1.0, "a soloist's held note holds its level (" + std::to_string(drift) + " dB)");
        // A section's detuned players beat, as real ones do (most when muted: the fundamental
        // dominates), so compare seconds, not instants.
        const auto y = sustain<StringsEngine, S>(64, 0.6, 7.0, 48000.0, {});
        const double sway = rmsDb(y, 4.0, 7.0, 48000.0) - rmsDb(y, 1.0, 4.0, 48000.0);
        expect(std::abs(sway) < 3.0, "a section's held note doesn't fade (" + std::to_string(sway) + " dB)");
    }
    {
        const auto soft = sustain<StringsEngine, S>(69, 0.3, 2.0, 48000.0, solo), loud = sustain<StringsEngine, S>(69, 1.0, 2.0, 48000.0, solo);
        const double louder = rmsDb(loud, 1.0, 2.0, 48000.0) - rmsDb(soft, 1.0, 2.0, 48000.0);
        const double brighter = tilt(loud, 1.0, 2.0, 48000.0) - tilt(soft, 1.0, 2.0, 48000.0);
        expect(louder > 6.0 && brighter > 3.0, "CC1 is the bow: louder by " + std::to_string(louder) + " dB and brighter by " +
                                                     std::to_string(brighter) + " dB");
    }
    {
        const auto muted = sustain<StringsEngine, S>(69, 0.6, 2.0, 48000.0, {}), open = sustain<StringsEngine, S>(69, 0.6, 2.0, 48000.0, {{S::mute, 0.0}});
        const double darker = tilt(open, 1.0, 2.0, 48000.0) - tilt(muted, 1.0, 2.0, 48000.0);
        expect(darker > 3.0, "con sordino is darker by " + std::to_string(darker) + " dB");
    }
    {
        // A soloist's reversals; a section's fall out of step and shimmer instead, as real ones do.
        const auto plain = sustain<StringsEngine, S>(60, 0.6, 2.5, 48000.0, solo);
        auto reversing = solo;
        reversing.push_back({S::tremolo, 1.0});
        const auto tremolo = sustain<StringsEngine, S>(60, 0.6, 2.5, 48000.0, reversing);
        // The string rings through each reversal: a pulse, not a collapse and rebuild every stroke.
        const double pulse = flutter(tremolo, 1.0, 2.5, 48000.0), steady = flutter(plain, 1.0, 2.5, 48000.0);
        const double drop = rmsDb(plain, 1.0, 2.5, 48000.0) - rmsDb(tremolo, 1.0, 2.5, 48000.0);
        expect(pulse > steady + 1.5 && drop < 4.0, "tremolo is bow reversals: the level pulses " + std::to_string(pulse) +
                                                       " dB (held " + std::to_string(steady) + "), " + std::to_string(drop) +
                                                       " dB under a held note");
        const auto dark = sustain<StringsEngine, S>(69, 0.6, 2.0, 48000.0, {{S::section, 1.0}, {S::vibratoDepth, 0.0}, {S::brightness, 0.25}});
        const auto bright = sustain<StringsEngine, S>(69, 0.6, 2.0, 48000.0, {{S::section, 1.0}, {S::vibratoDepth, 0.0}, {S::brightness, 2.0}});
        const double range = tilt(bright, 1.0, 2.0, 48000.0) - tilt(dark, 1.0, 2.0, 48000.0);
        expect(range > 10.0, "Brightness 0.25 to 2 brightens the strings by " + std::to_string(range) + " dB");
    }
    {
        std::vector<float> right;
        const auto left = sustain<StringsEngine, S>(60, 0.6, 1.0, 48000.0, {{S::stereoWidth, 0.0}}, &right);
        expect(left == right, "strings at Stereo Width 0 are exactly mono");
    }
    {
        std::size_t ended = 0;
        sustain<StringsEngine, S>(60, 0.6, 3.0, 48000.0, {}, nullptr, &ended, 1.0);
        expect(ended == 1, "a released string note ends within two seconds");
        expect(sustain<StringsEngine, S>(57, 0.6, 1.0, 48000.0, {}) == sustain<StringsEngine, S>(57, 0.6, 1.0, 48000.0, {}),
               "string renders repeat exactly");
    }
}

void hornTests() {
    using H = HornParameterId;
    for (const double rate : {48000.0, 96000.0}) {
        for (const int key : {41, 53, 65, 77}) {
            const auto x = sustain<HornEngine, H>(key, 0.6, 2.0, rate, {});
            const double off = centsOff(x, 1.0, 2.0, rate, frequency(key));
            expect(std::abs(off) <= 6.0, "horn key " + std::to_string(key) + " in tune at " + std::to_string(static_cast<int>(rate)) +
                                             " Hz: " + std::to_string(off) + " cents");
        }
    }
    {
        const auto x = sustain<HornEngine, H>(60, 0.6, 7.0, 48000.0, {});
        expect(std::abs(rmsDb(x, 6.0, 7.0, 48000.0) - rmsDb(x, 1.0, 2.0, 48000.0)) < 1.0, "a held horn note holds its level");
    }
    {
        const auto soft = sustain<HornEngine, H>(60, 0.3, 2.0, 48000.0, {}), loud = sustain<HornEngine, H>(60, 1.0, 2.0, 48000.0, {});
        const double louder = rmsDb(loud, 1.0, 2.0, 48000.0) - rmsDb(soft, 1.0, 2.0, 48000.0);
        const double brighter = tilt(loud, 1.0, 2.0, 48000.0) - tilt(soft, 1.0, 2.0, 48000.0);
        expect(louder > 4.0 && brighter > 4.0, "CC1 is breath: louder by " + std::to_string(louder) + " dB and brassier by " +
                                                    std::to_string(brighter) + " dB");
        const auto stopped = sustain<HornEngine, H>(60, 0.6, 2.0, 48000.0, {{H::stopped, 1.0}}), open = sustain<HornEngine, H>(60, 0.6, 2.0, 48000.0, {});
        expect(rmsDb(stopped, 1.0, 2.0, 48000.0) < rmsDb(open, 1.0, 2.0, 48000.0) - 3.0 &&
                   tilt(stopped, 1.0, 2.0, 48000.0) > tilt(open, 1.0, 2.0, 48000.0) + 6.0,
               "a stopped horn is quieter and thinner");
    }
    {
        std::vector<float> right;
        const auto left = sustain<HornEngine, H>(60, 0.6, 1.0, 48000.0, {{H::stereoWidth, 0.0}}, &right);
        expect(left == right, "horns at Stereo Width 0 are exactly mono");
        std::size_t ended = 0;
        sustain<HornEngine, H>(60, 0.6, 2.0, 48000.0, {}, nullptr, &ended, 1.0);
        expect(ended == 1, "a released horn note ends within a second");
        expect(sustain<HornEngine, H>(55, 0.6, 1.0, 48000.0, {}) == sustain<HornEngine, H>(55, 0.6, 1.0, 48000.0, {}),
               "horn renders repeat exactly");
    }
}

// piano.py writes each clip's CC1 curve again as per-note Pressure, the expression Bitwig imports
// from a .dawproject, so Pressure alone must carry the dynamics.
template <class Engine>
double pressed(int key, double pressure) {
    Engine engine;
    engine.prepare(48000.0);
    const auto x = render(engine, 2.0, 48000.0, [&](std::size_t f) {
        if (f == 0) {
            engine.noteOn(note(key, 4), 0.7);
            engine.setNotePressure(note(key, 4), pressure);
        }
    });
    return rmsDb(x, 1.0, 2.0, 48000.0);
}

void pressureTests() {
    const double flute = pressed<FluteEngine>(69, 1.0) - pressed<FluteEngine>(69, 0.3);
    const double strings = pressed<StringsEngine>(69, 1.0) - pressed<StringsEngine>(69, 0.3);
    const double horn = pressed<HornEngine>(60, 1.0) - pressed<HornEngine>(60, 0.3);
    expect(flute > 4.0 && strings > 4.0 && horn > 4.0, "per-note Pressure is the dynamic without CC1: flute +" +
                                                            std::to_string(flute) + ", strings +" + std::to_string(strings) +
                                                            ", horn +" + std::to_string(horn) + " dB");
}

} // namespace

int main() {
    harpTests();
    fluteTests();
    stringsTests();
    hornTests();
    pressureTests();
    std::printf("%s\n", failures ? "FAILED" : "all orchestra checks passed");
    return failures != 0;
}
