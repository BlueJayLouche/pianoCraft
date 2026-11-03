// The final act's promises in docs/old-house-needs.md: in tune, harp strings that keep ringing
// through a glissando, harmonics an octave up, mono-safe width; a flute whose held notes hold, whose
// overlapping notes slur, and whose CC1 is breath; strings and horns that sustain, whose CC1 is
// bow or breath, with their articulations. Every render must repeat exactly.
#include "engine/FluteEngine.hpp"
#include "engine/HarpEngine.hpp"
#include "engine/BrassEngine.hpp"
#include "engine/ReedEngine.hpp"
#include "engine/StringsEngine.hpp"
#include "engine/StruckEngine.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

using pianocraft::FluteEngine;
using pianocraft::FluteParameterId;
using pianocraft::HarpEngine;
using pianocraft::HarpParameterId;
using pianocraft::BrassParameterId;
using pianocraft::HornEngine;
using pianocraft::TromboneEngine;
using pianocraft::TrumpetEngine;
using pianocraft::TubaEngine;
using pianocraft::StringsEngine;
using pianocraft::StringsParameterId;
using pianocraft::NoteAddress;
using pianocraft::ClarinetEngine;
using pianocraft::OboeEngine;
using pianocraft::ReedParameterId;
using pianocraft::CelestaEngine;
using pianocraft::CrotalesEngine;
using pianocraft::StruckParameterId;

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
        damped.setParameter(HarpParameterId::sympatheticLevel, 0.0); // the other strings ring on, rightly
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
    {
        // A C3 damped by the hand at 0.35 s: its octave rings on in sympathy; C#4, which the pedals
        // (C major until something else is played) don't sound, stays quiet.
        const auto damped = [](double sympathy) {
            HarpEngine harp;
            harp.prepare(48000.0);
            harp.setParameter(HarpParameterId::damping, 1.0);
            harp.setParameter(HarpParameterId::sympatheticLevel, sympathy);
            return render(harp, 2.0, 48000.0, [&](std::size_t f) {
                if (f == 0) harp.noteOn(note(48), 0.8);
                if (f == 16768) harp.noteOff(note(48), 0.0); // 0.35 s, on a block
            });
        };
        const auto ringing = damped(1.0), silent = damped(0.0);
        const double octave = powerAt(ringing, 1.0, 2.0, 48000.0, frequency(60));
        const double unpedalled = powerAt(ringing, 1.0, 2.0, 48000.0, frequency(61));
        expect(10.0 * std::log10(octave / (powerAt(silent, 1.0, 2.0, 48000.0, frequency(60)) + 1e-30)) > 30.0 &&
                   10.0 * std::log10(unpedalled / octave) < -20.0,
               "a damped string's octave rings on in sympathy, a string the pedals don't sound doesn't");
        // Playing an F# moves the F pedal: then a damped F#3's octave rings, and F4 no longer does.
        HarpEngine harp;
        harp.prepare(48000.0);
        harp.setParameter(HarpParameterId::damping, 1.0);
        const auto moved = render(harp, 2.0, 48000.0, [&](std::size_t f) {
            if (f == 0) harp.noteOn(note(54), 0.8);
            if (f == 16768) harp.noteOff(note(54), 0.0);
        });
        expect(10.0 * std::log10(powerAt(moved, 1.0, 2.0, 48000.0, frequency(65)) / powerAt(moved, 1.0, 2.0, 48000.0, frequency(66))) < -20.0,
               "an F# moves the F pedal: the F# strings ring in sympathy, the F strings don't");
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
            // Read at the 2nd harmonic: a cello's low C radiates its fundamental 17-20 dB under its
            // strongest harmonic (the Iowa recordings; the body's air mode is above it).
            const double off = centsOff(x, 1.0, 2.0, rate, 2.0 * frequency(key));
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

// The brass: in tune, a held note holds, CC1 louder and brassier, the stopped horn or muted bell
// quieter and thinner, mono at width 0, a released note ends, renders repeat.
template <class Engine>
void brassTests(const std::string& name, std::vector<int> keys, const char* closed) {
    using H = BrassParameterId;
    const int middle = keys[keys.size() / 2];
    for (const double rate : {48000.0, 96000.0}) {
        for (const int key : keys) {
            const auto x = sustain<Engine, H>(key, 0.6, 2.0, rate, {});
            const double off = centsOff(x, 1.0, 2.0, rate, frequency(key));
            expect(std::abs(off) <= 6.0, name + " key " + std::to_string(key) + " in tune at " + std::to_string(static_cast<int>(rate)) +
                                             " Hz: " + std::to_string(off) + " cents");
        }
    }
    {
        const auto x = sustain<Engine, H>(middle, 0.6, 7.0, 48000.0, {});
        expect(std::abs(rmsDb(x, 6.0, 7.0, 48000.0) - rmsDb(x, 1.0, 2.0, 48000.0)) < 1.0, "a held " + name + " note holds its level");
    }
    {
        const auto soft = sustain<Engine, H>(middle, 0.3, 2.0, 48000.0, {}), loud = sustain<Engine, H>(middle, 1.0, 2.0, 48000.0, {});
        const double louder = rmsDb(loud, 1.0, 2.0, 48000.0) - rmsDb(soft, 1.0, 2.0, 48000.0);
        const double brighter = tilt(loud, 1.0, 2.0, 48000.0) - tilt(soft, 1.0, 2.0, 48000.0);
        expect(louder > 4.0 && brighter > 4.0, name + ": CC1 is breath, louder by " + std::to_string(louder) + " dB and brassier by " +
                                                    std::to_string(brighter) + " dB");
        const auto shut = sustain<Engine, H>(middle, 0.6, 2.0, 48000.0, {{H::stopped, 1.0}}), open = sustain<Engine, H>(middle, 0.6, 2.0, 48000.0, {});
        const double quieter = rmsDb(open, 1.0, 2.0, 48000.0) - rmsDb(shut, 1.0, 2.0, 48000.0);
        const double thinner = tilt(shut, 1.0, 2.0, 48000.0) - tilt(open, 1.0, 2.0, 48000.0);
        expect(quieter > 3.0 && thinner > 3.0, std::string(closed) + " is quieter by " + std::to_string(quieter) + " dB and thinner by " +
                                                   std::to_string(thinner) + " dB");
    }
    {
        std::vector<float> right;
        const auto left = sustain<Engine, H>(middle, 0.6, 1.0, 48000.0, {{H::stereoWidth, 0.0}}, &right);
        expect(left == right, name + " at Stereo Width 0 is exactly mono");
        std::size_t ended = 0;
        sustain<Engine, H>(middle, 0.6, 2.0, 48000.0, {}, nullptr, &ended, 1.0);
        expect(ended == 1, "a released " + name + " note ends within a second");
        expect(sustain<Engine, H>(middle - 5, 0.6, 1.0, 48000.0, {}) == sustain<Engine, H>(middle - 5, 0.6, 1.0, 48000.0, {}),
               name + " renders repeat exactly");
    }
}

// Each brass instrument has its own voice: the trumpet brightest, the tuba darkest, the
// trombone's blare at ff above the horn's.
void brassVoices() {
    const auto bright = [](auto engine, int key, double breath) {
        using E = decltype(engine);
        const auto x = sustain<E, BrassParameterId>(key, breath, 2.0, 48000.0, {});
        return tilt(x, 1.0, 2.0, 48000.0);
    };
    const double horn = bright(HornEngine{}, 60, 0.6), trumpet = bright(TrumpetEngine{}, 60, 0.6), trombone = bright(TromboneEngine{}, 60, 0.6),
                 tuba = bright(TubaEngine{}, 41, 0.6), hornLow = bright(HornEngine{}, 41, 0.6);
    expect(trumpet > trombone + 3.0 && trombone > horn + 3.0 && tuba < hornLow, "each brass its own bell at C4: trumpet " + std::to_string(trumpet) +
               ", trombone " + std::to_string(trombone) + ", horn " + std::to_string(horn) + " dB above 3 kHz; at F2 tuba " +
               std::to_string(tuba) + ", horn " + std::to_string(hornLow));
    // Two laws the Iowa recordings set (docs/brass.md): a low horn note's fundamental is weak beside
    // its harmonics around 300-500 Hz (15-18 dB under them at mf), and a note speaks at once (within
    // 10 dB of its level 5-20 ms after it starts, from C4 up).
    {
        const auto x = sustain<HornEngine, BrassParameterId>(41, 0.6, 2.0, 48000.0, {});
        double strongest = 0.0;
        for (int k = 3; k <= 8; ++k) {
            strongest = std::max(strongest, powerAt(x, 1.0, 2.0, 48000.0, k * frequency(41)));
        }
        const double under = 10.0 * std::log10(strongest / powerAt(x, 1.0, 2.0, 48000.0, frequency(41)));
        expect(under > 10.0, "a low horn note's fundamental is " + std::to_string(under) + " dB under its strongest harmonic");
    }
    {
        const auto x = sustain<HornEngine, BrassParameterId>(65, 0.6, 1.0, 48000.0, {});
        const double steady = rmsDb(x, 0.5, 1.0, 48000.0);
        double start = -1.0, speaks = -1.0;
        for (double t = 0.0; t < 0.3 && speaks < 0.0; t += 0.001) {
            const double level = rmsDb(x, t, t + 0.005, 48000.0);
            start = start < 0.0 && level > steady - 40.0 ? t : start;
            speaks = level > steady - 10.0 ? t : -1.0;
        }
        expect(start >= 0.0 && speaks - start <= 0.015, "a horn note speaks in " + std::to_string((speaks - start) * 1000.0) + " ms");
    }
}

// Oboe and clarinet: in tune, holding, CC1 louder and richer, each bore's signature, and slurs at pp.
template <class Engine>
void reedTests(const char* name, std::initializer_list<int> keys) {
    using R = ReedParameterId;
    const std::string instrument = name;
    const auto straight = std::vector<std::pair<R, double>>{{R::vibratoDepth, 0.0}};
    for (const double rate : {48000.0, 96000.0}) {
        for (const int key : keys) {
            const auto x = sustain<Engine, R>(key, 0.6, 2.0, rate, straight);
            const double off = centsOff(x, 1.0, 2.0, rate, frequency(key));
            expect(std::abs(off) <= 5.0, instrument + " key " + std::to_string(key) + " in tune at " +
                                             std::to_string(static_cast<int>(rate)) + " Hz: " + std::to_string(off) + " cents");
        }
    }
    const int middle = *(keys.begin() + 1);
    const auto held = sustain<Engine, R>(middle, 0.6, 7.0, 48000.0, straight);
    expect(std::abs(rmsDb(held, 6.0, 7.0, 48000.0) - rmsDb(held, 1.0, 2.0, 48000.0)) < 1.0, "a held " + instrument + " note holds its level");
    const auto soft = sustain<Engine, R>(middle, 0.2, 2.0, 48000.0, straight), loud = sustain<Engine, R>(middle, 1.0, 2.0, 48000.0, straight);
    const double louder = rmsDb(loud, 1.0, 2.0, 48000.0) - rmsDb(soft, 1.0, 2.0, 48000.0);
    const double richer = tilt(loud, 1.0, 2.0, 48000.0) - tilt(soft, 1.0, 2.0, 48000.0);
    expect(louder > 12.0 && richer > 6.0, instrument + ": CC1 is breath, " + std::to_string(louder) + " dB louder and " +
                                              std::to_string(richer) + " dB richer from 0.2 to 1");
    expect(sustain<Engine, R>(middle, 0.6, 1.0, 48000.0, {}) == sustain<Engine, R>(middle, 0.6, 1.0, 48000.0, {}),
           instrument + " renders repeat exactly");
    // Touching notes slur even at pp: whether a note is speaking is judged before the dynamic.
    Engine reed;
    reed.prepare(48000.0);
    reed.controlChange(0, 1, 0.15);
    reed.setParameter(R::vibratoDepth, 0.0);
    std::size_t endedAtSlur = 0;
    const auto x = render(reed, 1.0, 48000.0, [&](std::size_t f) {
        if (f == 0) reed.noteOn(note(middle, 1), 0.7);
        if (f == 24000) {
            reed.noteOff(note(middle, 1), 0.0);
            reed.noteOn(note(middle + 2, 2), 0.7);
        }
    });
    double quietest = 0.0;
    for (double t = 0.5; t < 0.6; t += 0.005) {
        quietest = std::min(quietest, rmsDb(x, t, t + 0.01, 48000.0) - rmsDb(x, 0.35, 0.5, 48000.0));
    }
    expect(quietest > -6.0, instrument + ": touching notes slur at pp (dip " + std::to_string(quietest) + " dB)");
}

// A slur is keys changing the tube, not a glide: in an octave slur, the tritone between the two
// notes must stay well under both while the keys move (a 12 ms glide put it 7 dB over them).
template <class Engine>
double between(const char* name) {
    Engine wind;
    wind.prepare(48000.0);
    wind.controlChange(0, 1, 0.6);
    wind.setParameter(pianocraft::WindParameterId::vibratoDepth, 0.0);
    const auto x = render(wind, 1.0, 48000.0, [&](std::size_t f) {
        if (f == 0) wind.noteOn(note(62, 1), 0.7);
        if (f == 24000) wind.noteOn(note(74, 2), 0.7);
        if (f == 24512) wind.noteOff(note(62, 1), 0.0);
    });
    double worst = -1.0e9;
    for (double t = 0.49; t <= 0.52; t += 0.002) {
        const double ends = std::max(powerAt(x, t, t + 0.02, 48000.0, frequency(62)), powerAt(x, t, t + 0.02, 48000.0, frequency(74)));
        worst = std::max(worst, 10.0 * std::log10(powerAt(x, t, t + 0.02, 48000.0, frequency(68)) / ends));
    }
    expect(worst < -6.0, std::string(name) + ": a slur changes the tube, no glide (the pitch between " + std::to_string(worst) + " dB re the notes)");
    return worst;
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
    const double oboe = pressed<OboeEngine>(69, 1.0) - pressed<OboeEngine>(69, 0.3);
    const double clarinet = pressed<ClarinetEngine>(62, 1.0) - pressed<ClarinetEngine>(62, 0.3);
    expect(flute > 4.0 && strings > 4.0 && horn > 4.0 && oboe > 4.0 && clarinet > 4.0,
           "per-note Pressure is the dynamic without CC1: flute +" + std::to_string(flute) + ", strings +" + std::to_string(strings) +
               ", horn +" + std::to_string(horn) + ", oboe +" + std::to_string(oboe) + ", clarinet +" + std::to_string(clarinet) + " dB");
}

} // namespace

// Struck metal: in tune, velocity the dynamic (louder and brighter), exact mono at width 0, renders that
// repeat; the celesta's dampers fall at note-off unless CC64 holds them, the crotales ring on.
template <class Engine>
std::vector<float> strike(int key, double velocity, double seconds, double rate, double releaseAt,
                          std::vector<std::pair<StruckParameterId, double>> settings = {}, bool pedal = false,
                          std::vector<float>* right = nullptr) {
    Engine engine;
    engine.prepare(rate);
    for (const auto& [id, value] : settings) {
        engine.setParameter(id, value);
    }
    if (pedal) {
        engine.controlChange(0, 64, 1.0);
    }
    const auto release = static_cast<std::size_t>(releaseAt * rate) / 64 * 64;
    return render(engine, seconds, rate, [&](std::size_t f) {
        if (f == 0) engine.noteOn(note(key), velocity);
        if (f == release) engine.noteOff(note(key), 0.0);
    }, right);
}

template <class Engine>
void struckTests(const std::string& name, int key, bool dampers) {
    for (double rate : {48000.0, 96000.0}) {
        const auto x = strike<Engine>(key, 0.7, 2.0, rate, 2.0);
        const double off = centsOff(x, 0.5, 2.0, rate, frequency(key));
        expect(std::abs(off) <= 1.0, name + " in tune at " + std::to_string(static_cast<int>(rate)) + " Hz: " + std::to_string(off) + " cents");
    }
    const double rate = 48000.0;
    const auto released = strike<Engine>(key, 0.7, 1.0, rate, 0.1);
    const auto pedalled = strike<Engine>(key, 0.7, 1.0, rate, 0.1, {}, true);
    const double drop = rmsDb(released, 0.7, 1.0, rate) - rmsDb(released, 0.0, 0.1, rate);
    const double held = rmsDb(pedalled, 0.7, 1.0, rate) - rmsDb(pedalled, 0.0, 0.1, rate);
    if (dampers) {
        expect(drop < -50.0 && held > -25.0, name + " damped at note-off (" + std::to_string(drop) + " dB), held by CC64 (" +
                                                 std::to_string(held) + " dB)");
    } else {
        const auto damped = strike<Engine>(key, 0.7, 1.0, rate, 0.1, {{StruckParameterId::damping, 1.0}});
        const double hand = rmsDb(damped, 0.7, 1.0, rate) - rmsDb(damped, 0.0, 0.1, rate);
        expect(drop > -25.0 && hand < -50.0, name + " ring on after note-off (" + std::to_string(drop) +
                                                 " dB); Note-Off Damping stops them (" + std::to_string(hand) + " dB)");
    }
    const auto soft = strike<Engine>(key, 0.35, 0.5, rate, 0.5);
    const auto loud = strike<Engine>(key, 0.9, 0.5, rate, 0.5);
    const double louder = rmsDb(loud, 0.0, 0.3, rate) - rmsDb(soft, 0.0, 0.3, rate);
    const double brighter = tilt(loud, 0.0, 0.1, rate) - tilt(soft, 0.0, 0.1, rate);
    expect(louder > 15.0 && brighter > 3.0, name + " ff over pp: " + std::to_string(louder) + " dB louder, " +
                                                std::to_string(brighter) + " dB brighter");
    std::vector<float> right;
    const auto left = strike<Engine>(key, 0.7, 0.3, rate, 0.3, {{StruckParameterId::stereoWidth, 0.0}}, false, &right);
    const auto again = strike<Engine>(key, 0.7, 0.3, rate, 0.3, {{StruckParameterId::stereoWidth, 0.0}});
    expect(left == right && left == again, name + ": Stereo Width 0 is exact mono, and renders repeat");
}

int main() {
    harpTests();
    fluteTests();
    stringsTests();
    brassTests<HornEngine>("horn", {41, 53, 65, 77}, "a stopped horn");
    brassTests<TrumpetEngine>("trumpet", {52, 60, 67, 74, 84}, "a muted trumpet");
    brassTests<TromboneEngine>("trombone", {40, 48, 55, 62, 70, 77}, "a muted trombone");
    brassTests<TubaEngine>("tuba", {26, 33, 41, 48, 55, 65}, "a muted tuba");
    brassVoices();
    between<FluteEngine>("flute");
    between<OboeEngine>("oboe");
    between<ClarinetEngine>("clarinet");
    reedTests<OboeEngine>("oboe", {58, 69, 81, 93});
    reedTests<ClarinetEngine>("clarinet", {50, 62, 76, 94});
    {
        // Each bore's signature in its low register: the clarinet's cylinder sounds odd harmonics
        // only; the oboe's cone every one, its second as strong as its first.
        using R = ReedParameterId;
        const auto clarinet = sustain<ClarinetEngine, R>(50, 0.8, 2.0, 48000.0, {{R::vibratoDepth, 0.0}});
        const double even = 10.0 * std::log10(powerAt(clarinet, 1.0, 2.0, 48000.0, 2.0 * frequency(50)) /
                                              powerAt(clarinet, 1.0, 2.0, 48000.0, 3.0 * frequency(50)));
        const auto oboe = sustain<OboeEngine, R>(58, 0.8, 2.0, 48000.0, {{R::vibratoDepth, 0.0}});
        const double second = 10.0 * std::log10(powerAt(oboe, 1.0, 2.0, 48000.0, 2.0 * frequency(58)) /
                                                powerAt(oboe, 1.0, 2.0, 48000.0, frequency(58)));
        expect(even < -20.0 && second > -6.0, "the clarinet's 2nd harmonic is " + std::to_string(even) +
                                                  " dB under its 3rd; the oboe's 2nd is " + std::to_string(second) + " dB re its 1st");
    }
    pressureTests();
    struckTests<CelestaEngine>("celesta", 72, true);
    struckTests<CrotalesEngine>("crotales", 96, false);
    std::printf("%s\n", failures ? "FAILED" : "all orchestra checks passed");
    return failures != 0;
}
