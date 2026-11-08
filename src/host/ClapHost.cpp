#include "../engine/FluteEngine.hpp"
#include "../engine/HarpEngine.hpp"
#include "../engine/BrassEngine.hpp"
#include "../engine/PianoEngine.hpp"
#include "../engine/ReedEngine.hpp"
#include "../engine/StringsEngine.hpp"
#include "../engine/StruckEngine.hpp"
#include "ClapGui.hpp"
#include "GuiEdits.hpp"

#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using pianocraft::FluteEngine;
using pianocraft::FluteParameterId;
using pianocraft::HarpEngine;
using pianocraft::HarpParameterId;
using pianocraft::BrassParameterId;
using pianocraft::HornEngine;
using pianocraft::NoteAddress;
using pianocraft::ParameterId;
using pianocraft::PianoEngine;
using pianocraft::ReedParameterId;
using pianocraft::StringsEngine;
using pianocraft::StringsParameterId;
using pianocraft::StruckParameterId;

struct ParameterSpec {
    clap_id id;
    const char* name;
    const char* module;
    double minimum;
    double maximum;
    double defaultValue;
    clap_param_info_flags flags;
    const char* const* choices = nullptr; // names of a stepped parameter's values, from 0
};

constexpr clap_param_info_flags continuousFlags = CLAP_PARAM_IS_AUTOMATABLE;
constexpr clap_param_info_flags choiceFlags =
    CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM;
constexpr clap_param_info_flags steppedFlags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
constexpr clap_param_info_flags perNoteFlags = continuousFlags | CLAP_PARAM_IS_MODULATABLE |
    CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID | CLAP_PARAM_IS_MODULATABLE_PER_KEY |
    CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL | CLAP_PARAM_IS_MODULATABLE_PER_PORT;
constexpr std::array<const char*, 3> micPositionNames{"Spaced Pair", "Coincident", "Player"};
constexpr std::array<const char*, 2> offOnNames{"Off", "On"};

template <class Id>
constexpr clap_id pid(Id id) noexcept
{
    return static_cast<clap_id>(id);
}

struct RemotePage {
    const char* name;
    std::array<clap_id, CLAP_REMOTE_CONTROLS_COUNT> parameters;
};

// Built-in voicings for Bitwig's browser (CLAP preset discovery): every parameter at its default
// except those listed (id 0 is an unused slot). Bitwig lists preset files, not presets held inside
// the plugin, so each one ships as a marker file <loadKey>.pianocraft in the bundle's
// Resources/Presets; the values stay here.
struct Preset {
    const char* name;
    const char* loadKey;
    const char* description;
    std::array<std::pair<clap_id, double>, 4> changes;
};

constexpr const char* const pluginFeatures[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_SYNTHESIZER,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

// Each instrument is a plugin of its own in the one bundle: its engine, parameters, Bitwig
// remote-control pages and built-in presets.
struct Piano {
    using Engine = PianoEngine;
    using Id = ParameterId;
    static constexpr const char* id = "com.pianocraft.upright";
    static constexpr const char* name = "PianoCraft Upright";
    static constexpr const char* description = "Original upright piano instrument";
    static constexpr const char* section = "Piano";

    // Listed by group; hosts identify parameters by id, so the order is free.
    static constexpr std::array<ParameterSpec, 16> parameters{{
    {pid(ParameterId::hammerHardness), "Hammer Hardness", "Hammer", 0.25, 2.0, 1.0, perNoteFlags},
    {pid(ParameterId::dynamicRange), "Dynamic Range", "Hammer", 0.0, 2.0, 1.0, continuousFlags},
    {pid(ParameterId::velocityBrightness), "Velocity Brightness", "Hammer", 0.0, 2.0, 1.0, continuousFlags},
    {pid(ParameterId::hammerNoise), "Hammer Noise", "Hammer", 0.0, 2.0, 1.0, perNoteFlags},
    {pid(ParameterId::sustainLength), "Sustain Length", "Strings", 0.25, 4.0, 1.0, continuousFlags},
    {pid(ParameterId::damperRelease), "Damper Release", "Strings", 0.25, 4.0, 1.0, continuousFlags},
    {pid(ParameterId::unisonDetune), "Unison Detune", "Strings", 0.0, 3.0, 1.0, continuousFlags},
    {pid(ParameterId::aftersound), "Aftersound", "Strings", 0.0, 2.0, 1.0, continuousFlags},
    {pid(ParameterId::sympatheticLevel), "Sympathetic Level", "Strings", 0.0, 1.0, 0.35, continuousFlags},
    {pid(ParameterId::masterTune), "Master Tune", "Tuning", 415.0, 466.0, 440.0, continuousFlags},
    {pid(ParameterId::stretch), "Stretch", "Tuning", 0.0, 2.0, 1.0, continuousFlags},
    {pid(ParameterId::age), "Age", "Age", 0.0, 1.0, 0.0, continuousFlags},
    {pid(ParameterId::bodyLevel), "Body Level", "Mix", 0.0, 2.0, 1.0, continuousFlags},
    {pid(ParameterId::micPosition), "Mic Position", "Mix", 0.0, 2.0, 0.0, choiceFlags, micPositionNames.data()},
    {pid(ParameterId::stereoWidth), "Stereo Width", "Mix", 0.0, 2.0, 1.0, continuousFlags},
    {pid(ParameterId::outputGain), "Output Gain", "Mix", 0.0, 2.0, 0.7, continuousFlags},
}};

    static constexpr std::array<RemotePage, 2> pages{{
    {"Main", {pid(ParameterId::hammerHardness), pid(ParameterId::dynamicRange),
              pid(ParameterId::velocityBrightness), pid(ParameterId::sustainLength),
              pid(ParameterId::sympatheticLevel), pid(ParameterId::bodyLevel),
              pid(ParameterId::stereoWidth), pid(ParameterId::outputGain)}},
    {"Voicing", {pid(ParameterId::unisonDetune), pid(ParameterId::aftersound),
                 pid(ParameterId::damperRelease), pid(ParameterId::hammerNoise),
                 pid(ParameterId::micPosition), pid(ParameterId::masterTune), pid(ParameterId::stretch),
                 pid(ParameterId::age)}},
}};

    // The Old House score bounces mono, so its wear stages use Stereo Width 0.
    static constexpr std::array<Preset, 4> presets{{
    {"Calibrated Upright", "calibrated", "The recorded upright as measured, spaced pair, stereo",
     {{{pid(ParameterId::stereoWidth), 1.0}, {pid(ParameterId::age), 0.0}}}},
    {"Whole (Mono)", "whole-mono", "Unworn, mono-safe for a single emitter",
     {{{pid(ParameterId::stereoWidth), 0.0}, {pid(ParameterId::age), 0.0}}}},
    {"Lightly Worn (Mono)", "lightly-worn-mono", "Age 0.3, mono-safe for a single emitter",
     {{{pid(ParameterId::stereoWidth), 0.0}, {pid(ParameterId::age), 0.3}}}},
    {"Most Worn (Mono)", "most-worn-mono", "Age 1, mono-safe for a single emitter",
     {{{pid(ParameterId::stereoWidth), 0.0}, {pid(ParameterId::age), 1.0}}}},
}};
};

// The harp and flute have no reference corpus yet: their laws are analytic (docs/harp-and-flute.md).
struct Harp {
    using Engine = HarpEngine;
    using Id = HarpParameterId;
    static constexpr const char* id = "com.pianocraft.harp";
    static constexpr const char* name = "PianoCraft Harp";
    static constexpr const char* description = "Original concert harp instrument";
    static constexpr const char* section = "Harp";
    static constexpr std::array<ParameterSpec, 11> parameters{{
        {pid(HarpParameterId::pluckPosition), "Pluck Position", "Pluck", 0.03, 0.5, 0.35, perNoteFlags},
        {pid(HarpParameterId::presDeLaTable), "Pres de la Table", "Pluck", 0.0, 1.0, 0.0, continuousFlags},
        {pid(HarpParameterId::harmonics), "Harmonics", "Pluck", 0.0, 1.0, 0.0, choiceFlags, offOnNames.data()},
        {pid(HarpParameterId::brightness), "Brightness", "Pluck", 0.25, 2.0, 1.0, perNoteFlags},
        {pid(HarpParameterId::damping), "Note-Off Damping", "Strings", 0.0, 1.0, 0.0, continuousFlags},
        {pid(HarpParameterId::sustainLength), "Sustain Length", "Strings", 0.25, 4.0, 1.0, continuousFlags},
        {pid(HarpParameterId::sympatheticLevel), "Sympathetic Level", "Strings", 0.0, 2.0, 1.0, continuousFlags},
        {pid(HarpParameterId::masterTune), "Master Tune", "Tuning", 415.0, 466.0, 440.0, continuousFlags},
        {pid(HarpParameterId::bodyLevel), "Body Level", "Mix", 0.0, 2.0, 1.0, continuousFlags},
        {pid(HarpParameterId::stereoWidth), "Stereo Width", "Mix", 0.0, 1.0, 1.0, continuousFlags},
        {pid(HarpParameterId::outputGain), "Output Gain", "Mix", 0.0, 2.0, 0.7, continuousFlags},
    }};
    static constexpr std::array<RemotePage, 2> pages{{
        {"Main", {pid(HarpParameterId::pluckPosition), pid(HarpParameterId::presDeLaTable),
                  pid(HarpParameterId::harmonics), pid(HarpParameterId::brightness),
                  pid(HarpParameterId::damping), pid(HarpParameterId::sustainLength),
                  pid(HarpParameterId::stereoWidth), pid(HarpParameterId::outputGain)}},
        {"Strings", {pid(HarpParameterId::sympatheticLevel), pid(HarpParameterId::bodyLevel),
                     pid(HarpParameterId::masterTune), CLAP_INVALID_ID, CLAP_INVALID_ID, CLAP_INVALID_ID,
                     CLAP_INVALID_ID, CLAP_INVALID_ID}},
    }};
    static constexpr std::array<Preset, 4> presets{{
        {"Concert Harp", "harp-concert", "Ordinary plucks, strings let ring, stereo",
         {{{pid(HarpParameterId::stereoWidth), 1.0}}}},
        {"Harp (Mono)", "harp-mono", "Ordinary plucks, mono-safe for a single emitter",
         {{{pid(HarpParameterId::stereoWidth), 0.0}}}},
        {"Pres de la Table (Mono)", "harp-pres-de-la-table-mono", "Plucked at the soundboard: thin and metallic, mono",
         {{{pid(HarpParameterId::stereoWidth), 0.0}, {pid(HarpParameterId::presDeLaTable), 1.0}}}},
        {"Harmonics (Mono)", "harp-harmonics-mono", "Octave harmonics at the sounding pitch, mono",
         {{{pid(HarpParameterId::stereoWidth), 0.0}, {pid(HarpParameterId::harmonics), 1.0}}}},
    }};
};

struct Flute {
    using Engine = FluteEngine;
    using Id = FluteParameterId;
    static constexpr const char* id = "com.pianocraft.flute";
    static constexpr const char* name = "PianoCraft Flute";
    static constexpr const char* description = "Original solo flute instrument: CC1 is breath";
    static constexpr const char* section = "Flute";
    static constexpr std::array<ParameterSpec, 7> parameters{{
        {pid(FluteParameterId::breathNoise), "Breath Noise", "Breath", 0.0, 2.0, 1.0, continuousFlags},
        {pid(FluteParameterId::brightness), "Brightness", "Breath", 0.25, 2.0, 1.0, perNoteFlags},
        {pid(FluteParameterId::vibratoDepth), "Vibrato Depth", "Vibrato", 0.0, 2.0, 1.0, continuousFlags},
        {pid(FluteParameterId::vibratoRate), "Vibrato Rate", "Vibrato", 3.0, 8.0, 5.0, continuousFlags},
        {pid(FluteParameterId::legatoTime), "Legato Time", "Phrasing", 0.0, 0.2, 0.012, continuousFlags},
        {pid(FluteParameterId::masterTune), "Master Tune", "Tuning", 415.0, 466.0, 440.0, continuousFlags},
        {pid(FluteParameterId::outputGain), "Output Gain", "Mix", 0.0, 2.0, 0.7, continuousFlags},
    }};
    static constexpr std::array<RemotePage, 1> pages{{
        {"Main", {pid(FluteParameterId::breathNoise), pid(FluteParameterId::brightness),
                  pid(FluteParameterId::vibratoDepth), pid(FluteParameterId::vibratoRate),
                  pid(FluteParameterId::legatoTime), pid(FluteParameterId::masterTune),
                  pid(FluteParameterId::outputGain), CLAP_INVALID_ID}},
    }};
    static constexpr std::array<Preset, 2> presets{{
        {"Solo Flute", "flute-solo", "Breath on CC1, vibrato after the note speaks", {}},
        {"Veiled Flute", "flute-veiled", "Breathier and darker, little vibrato: the low Faune register",
         {{{pid(FluteParameterId::breathNoise), 1.8}, {pid(FluteParameterId::brightness), 0.7},
           {pid(FluteParameterId::vibratoDepth), 0.4}}}},
    }};
};

// Oboe and clarinet: reed waveguides played as the flute is (docs/oboe-and-clarinet.md).
template <class E>
struct Reed {
    using Engine = E;
    using Id = ReedParameterId;
    static constexpr std::array<ParameterSpec, 8> parametersWith(double vibrato) {
        return {{
            {pid(ReedParameterId::breathNoise), "Breath Noise", "Breath", 0.0, 2.0, 1.0, continuousFlags},
            {pid(ReedParameterId::brightness), "Brightness", "Breath", 0.25, 2.0, 1.0, perNoteFlags},
            {pid(ReedParameterId::reed), "Reed", "Breath", 0.0, 2.0, 1.0, continuousFlags},
            {pid(ReedParameterId::vibratoDepth), "Vibrato Depth", "Vibrato", 0.0, 2.0, vibrato, continuousFlags},
            {pid(ReedParameterId::vibratoRate), "Vibrato Rate", "Vibrato", 3.0, 8.0, 5.0, continuousFlags},
            {pid(ReedParameterId::legatoTime), "Legato Time", "Phrasing", 0.0, 0.2, 0.012, continuousFlags},
            {pid(ReedParameterId::masterTune), "Master Tune", "Tuning", 415.0, 466.0, 440.0, continuousFlags},
            {pid(ReedParameterId::outputGain), "Output Gain", "Mix", 0.0, 2.0, 0.7, continuousFlags},
        }};
    }
    static constexpr std::array<RemotePage, 1> pages{{
        {"Main", {pid(ReedParameterId::breathNoise), pid(ReedParameterId::brightness), pid(ReedParameterId::reed),
                  pid(ReedParameterId::vibratoDepth), pid(ReedParameterId::vibratoRate),
                  pid(ReedParameterId::legatoTime), pid(ReedParameterId::masterTune),
                  pid(ReedParameterId::outputGain)}},
    }};
};

struct Oboe : Reed<pianocraft::OboeEngine> {
    static constexpr const char* id = "com.pianocraft.oboe";
    static constexpr const char* name = "PianoCraft Oboe";
    static constexpr const char* description = "Original solo oboe instrument: CC1 is breath";
    static constexpr const char* section = "Oboe";
    static constexpr auto parameters = parametersWith(0.6);
    static constexpr std::array<Preset, 2> presets{{
        {"Oboe", "oboe", "Breath on CC1, a gentle vibrato after the note speaks", {}},
        {"Oboe Dolce", "oboe-dolce", "Darker and sweeter, a softer reed, less vibrato",
         {{{pid(ReedParameterId::brightness), 0.75}, {pid(ReedParameterId::reed), 0.6}, {pid(ReedParameterId::vibratoDepth), 0.4}}}},
    }};
};

struct Clarinet : Reed<pianocraft::ClarinetEngine> {
    static constexpr const char* id = "com.pianocraft.clarinet";
    static constexpr const char* name = "PianoCraft Clarinet";
    static constexpr const char* description = "Original solo B-flat clarinet instrument: CC1 is breath";
    static constexpr const char* section = "Clarinet";
    static constexpr auto parameters = parametersWith(0.0);
    static constexpr std::array<Preset, 2> presets{{
        {"Clarinet", "clarinet", "Breath on CC1, straight tone", {}},
        {"Clarinet Dolce", "clarinet-dolce", "Darker and softer-edged",
         {{{pid(ReedParameterId::brightness), 0.75}, {pid(ReedParameterId::reed), 0.6}}}},
    }};
};

// Strings and horn: analytic too (docs/strings-and-horn.md). The score's strings are con sordino.
struct Strings {
    using Engine = StringsEngine;
    using Id = StringsParameterId;
    static constexpr const char* id = "com.pianocraft.strings";
    static constexpr const char* name = "PianoCraft Strings";
    static constexpr const char* description = "Original bowed string section: CC1 is the bow";
    static constexpr const char* section = "Strings";
    static constexpr std::array<ParameterSpec, 12> parameters{{
        {pid(StringsParameterId::mute), "Con Sordino", "Articulation", 0.0, 1.0, 1.0, choiceFlags, offOnNames.data()},
        {pid(StringsParameterId::sulTasto), "Sul Tasto", "Articulation", 0.0, 1.0, 0.0, continuousFlags},
        {pid(StringsParameterId::tremolo), "Tremolo", "Articulation", 0.0, 1.0, 0.0, choiceFlags, offOnNames.data()},
        {pid(StringsParameterId::harmonics), "Harmonics", "Articulation", 0.0, 1.0, 0.0, choiceFlags, offOnNames.data()},
        {pid(StringsParameterId::section), "Section Size", "Section", 1.0, 8.0, 6.0, steppedFlags},
        {pid(StringsParameterId::spread), "Section Spread", "Section", 0.0, 2.0, 1.0, continuousFlags},
        {pid(StringsParameterId::brightness), "Brightness", "Bow", 0.25, 2.0, 1.0, perNoteFlags},
        {pid(StringsParameterId::vibratoDepth), "Vibrato Depth", "Vibrato", 0.0, 2.0, 1.0, continuousFlags},
        {pid(StringsParameterId::vibratoRate), "Vibrato Rate", "Vibrato", 3.0, 8.0, 5.5, continuousFlags},
        {pid(StringsParameterId::masterTune), "Master Tune", "Tuning", 415.0, 466.0, 440.0, continuousFlags},
        {pid(StringsParameterId::stereoWidth), "Stereo Width", "Mix", 0.0, 1.0, 1.0, continuousFlags},
        {pid(StringsParameterId::outputGain), "Output Gain", "Mix", 0.0, 2.0, 0.7, continuousFlags},
    }};
    static constexpr std::array<RemotePage, 2> pages{{
        {"Main", {pid(StringsParameterId::mute), pid(StringsParameterId::sulTasto), pid(StringsParameterId::tremolo),
                  pid(StringsParameterId::harmonics), pid(StringsParameterId::section), pid(StringsParameterId::spread),
                  pid(StringsParameterId::vibratoDepth), pid(StringsParameterId::outputGain)}},
        {"Tone", {pid(StringsParameterId::brightness), pid(StringsParameterId::vibratoRate),
                  pid(StringsParameterId::stereoWidth), pid(StringsParameterId::masterTune), CLAP_INVALID_ID,
                  CLAP_INVALID_ID, CLAP_INVALID_ID, CLAP_INVALID_ID}},
    }};
    static constexpr std::array<Preset, 7> presets{{
        {"Strings Con Sordino", "strings-con-sordino", "A section of six per note, muted, stereo", {}},
        {"Strings Con Sordino (Mono)", "strings-con-sordino-mono", "A muted section, mono-safe for a single emitter",
         {{{pid(StringsParameterId::stereoWidth), 0.0}}}},
        {"Strings Sul Tasto (Mono)", "strings-sul-tasto-mono", "Muted, bowed over the fingerboard: soft and airy, mono",
         {{{pid(StringsParameterId::stereoWidth), 0.0}, {pid(StringsParameterId::sulTasto), 1.0}}}},
        {"Strings Tremolo (Mono)", "strings-tremolo-mono", "Muted, bowed tremolo, mono",
         {{{pid(StringsParameterId::stereoWidth), 0.0}, {pid(StringsParameterId::tremolo), 1.0}}}},
        {"Strings Harmonics (Mono)", "strings-harmonics-mono", "Muted harmonics at the sounding pitch, mono",
         {{{pid(StringsParameterId::stereoWidth), 0.0}, {pid(StringsParameterId::harmonics), 1.0}}}},
        {"Solo String Con Sordino (Mono)", "strings-solo-mono", "One muted player per note, mono",
         {{{pid(StringsParameterId::stereoWidth), 0.0}, {pid(StringsParameterId::section), 1.0}}}},
        {"Strings Senza Sordino", "strings-senza-sordino", "A section of six per note, unmuted, stereo",
         {{{pid(StringsParameterId::mute), 0.0}}}},
    }};
};

// The brass share one engine and one set of controls; what closes the bell is the horn player's
// hand or a straight mute (docs/brass.md).
template <class E>
struct Brass {
    using Engine = E;
    using Id = BrassParameterId;
    static constexpr std::array<ParameterSpec, 5> parametersWith(const char* closed) {
        return {{
            {pid(BrassParameterId::stopped), closed, "Articulation", 0.0, 1.0, 0.0, choiceFlags, offOnNames.data()},
            {pid(BrassParameterId::brightness), "Brightness", "Bell", 0.25, 2.0, 1.0, perNoteFlags},
            {pid(BrassParameterId::masterTune), "Master Tune", "Tuning", 415.0, 466.0, 440.0, continuousFlags},
            {pid(BrassParameterId::stereoWidth), "Stereo Width", "Mix", 0.0, 1.0, 1.0, continuousFlags},
            {pid(BrassParameterId::outputGain), "Output Gain", "Mix", 0.0, 2.0, 0.7, continuousFlags},
        }};
    }
    static constexpr std::array<RemotePage, 1> pages{{
        {"Main", {pid(BrassParameterId::stopped), pid(BrassParameterId::brightness), pid(BrassParameterId::masterTune),
                  pid(BrassParameterId::stereoWidth), pid(BrassParameterId::outputGain), CLAP_INVALID_ID,
                  CLAP_INVALID_ID, CLAP_INVALID_ID}},
    }};
};

struct Horn : Brass<HornEngine> {
    static constexpr const char* id = "com.pianocraft.horn";
    static constexpr const char* name = "PianoCraft Horn";
    static constexpr const char* description = "Original French horn section: CC1 is breath";
    static constexpr const char* section = "Horn";
    static constexpr auto parameters = parametersWith("Stopped");
    static constexpr std::array<Preset, 3> presets{{
        {"Horns", "horn", "Open horns, a player per note, stereo", {}},
        {"Distant Horn (Mono)", "horn-distant-mono", "Darker, the bell turned away: for soft, far-off calls, mono",
         {{{pid(BrassParameterId::stereoWidth), 0.0}, {pid(BrassParameterId::brightness), 0.6}}}},
        {"Stopped Horn (Mono)", "horn-stopped-mono", "The hand closing the bell: thin, nasal and metallic, mono",
         {{{pid(BrassParameterId::stereoWidth), 0.0}, {pid(BrassParameterId::stopped), 1.0}}}},
    }};
};

struct Trumpet : Brass<pianocraft::TrumpetEngine> {
    static constexpr const char* id = "com.pianocraft.trumpet";
    static constexpr const char* name = "PianoCraft Trumpet";
    static constexpr const char* description = "Original B-flat trumpet section: CC1 is breath";
    static constexpr const char* section = "Trumpet";
    static constexpr auto parameters = parametersWith("Straight Mute");
    static constexpr std::array<Preset, 2> presets{{
        {"Trumpets", "trumpet", "Open trumpets, a player per note, stereo: brassy at ff", {}},
        {"Muted Trumpet (Mono)", "trumpet-muted-mono", "A straight mute: quieter, thin and nasal, mono",
         {{{pid(BrassParameterId::stereoWidth), 0.0}, {pid(BrassParameterId::stopped), 1.0}}}},
    }};
};

struct Trombone : Brass<pianocraft::TromboneEngine> {
    static constexpr const char* id = "com.pianocraft.trombone";
    static constexpr const char* name = "PianoCraft Trombone";
    static constexpr const char* description = "Original tenor trombone section: CC1 is breath";
    static constexpr const char* section = "Trombone";
    static constexpr auto parameters = parametersWith("Straight Mute");
    static constexpr std::array<Preset, 2> presets{{
        {"Trombones", "trombone", "Open trombones, a player per note, stereo: brassy at ff", {}},
        {"Muted Trombone (Mono)", "trombone-muted-mono", "A straight mute: quieter, thin and nasal, mono",
         {{{pid(BrassParameterId::stereoWidth), 0.0}, {pid(BrassParameterId::stopped), 1.0}}}},
    }};
};

struct Tuba : Brass<pianocraft::TubaEngine> {
    static constexpr const char* id = "com.pianocraft.tuba";
    static constexpr const char* name = "PianoCraft Tuba";
    static constexpr const char* description = "Original tuba: CC1 is breath";
    static constexpr const char* section = "Tuba";
    static constexpr auto parameters = parametersWith("Straight Mute");
    static constexpr std::array<Preset, 2> presets{{
        {"Tuba", "tuba", "Open tuba, stereo", {}},
        {"Tuba (Mono)", "tuba-mono", "Open tuba, mono-safe for a single emitter",
         {{{pid(BrassParameterId::stereoWidth), 0.0}}}},
    }};
};

// Celesta and crotales: struck metal, one engine; the celesta's dampers fall at note-off, the crotales ring
// until they die away (docs/celesta-and-crotales.md).
template <class E>
struct Struck {
    using Engine = E;
    using Id = StruckParameterId;
    static constexpr std::array<ParameterSpec, 6> parametersWith(const char* striker, double damping) {
        return {{
            {pid(StruckParameterId::brightness), "Brightness", striker, 0.25, 2.0, 1.0, perNoteFlags},
            {pid(StruckParameterId::damping), "Note-Off Damping", striker, 0.0, 1.0, damping, continuousFlags},
            {pid(StruckParameterId::sustainLength), "Sustain Length", striker, 0.25, 4.0, 1.0, continuousFlags},
            {pid(StruckParameterId::masterTune), "Master Tune", "Tuning", 415.0, 466.0, 440.0, continuousFlags},
            {pid(StruckParameterId::stereoWidth), "Stereo Width", "Mix", 0.0, 1.0, 1.0, continuousFlags},
            {pid(StruckParameterId::outputGain), "Output Gain", "Mix", 0.0, 2.0, 0.7, continuousFlags},
        }};
    }
    static constexpr std::array<RemotePage, 1> pages{{
        {"Main", {pid(StruckParameterId::brightness), pid(StruckParameterId::damping), pid(StruckParameterId::sustainLength),
                  pid(StruckParameterId::masterTune), pid(StruckParameterId::stereoWidth), pid(StruckParameterId::outputGain),
                  CLAP_INVALID_ID, CLAP_INVALID_ID}},
    }};
};

struct Celesta : Struck<pianocraft::CelestaEngine> {
    static constexpr const char* id = "com.pianocraft.celesta";
    static constexpr const char* name = "PianoCraft Celesta";
    static constexpr const char* description = "Original celesta instrument";
    static constexpr const char* section = "Celesta";
    static constexpr auto parameters = parametersWith("Hammers", 1.0);
    static constexpr std::array<Preset, 2> presets{{
        {"Celesta", "celesta", "Felt hammers on steel bars, dampers at note-off (CC64 lifts them), stereo", {}},
        {"Celesta (Mono)", "celesta-mono", "Mono-safe for a single emitter",
         {{{pid(StruckParameterId::stereoWidth), 0.0}}}},
    }};
};

struct Crotales : Struck<pianocraft::CrotalesEngine> {
    static constexpr const char* id = "com.pianocraft.crotales";
    static constexpr const char* name = "PianoCraft Crotales";
    static constexpr const char* description = "Original crotales (antique cymbals) instrument";
    static constexpr const char* section = "Crotales";
    static constexpr auto parameters = parametersWith("Mallets", 0.0);
    static constexpr std::array<Preset, 2> presets{{
        {"Crotales", "crotales", "Bronze discs let ring, stereo", {}},
        {"Crotales (Mono)", "crotales-mono", "Mono-safe for a single emitter",
         {{{pid(StruckParameterId::stereoWidth), 0.0}}}},
    }};
};

// Set once in entryInit: the folder of preset marker files inside this bundle.
std::string presetDirectory;

template <class I>
const Preset* findPreset(std::uint32_t locationKind, const char* location) noexcept
{
    if (locationKind != CLAP_PRESET_DISCOVERY_LOCATION_FILE || location == nullptr) {
        return nullptr;
    }
    const char* slash = std::strrchr(location, '/');
    const char* file = slash != nullptr ? slash + 1 : location;
    for (const auto& preset : I::presets) {
        const std::size_t length = std::strlen(preset.loadKey);
        if (std::strncmp(file, preset.loadKey, length) == 0 && std::strcmp(file + length, ".pianocraft") == 0) {
            return &preset;
        }
    }
    return nullptr;
}

template <class I>
constexpr std::size_t invalidParameterIndex = I::parameters.size();
template <class I>
constexpr std::size_t stateSize = 12 + I::parameters.size() * 12;
constexpr std::uint32_t stateVersion = 1;
static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == 8);
static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

template <class I>
std::size_t parameterIndex(clap_id id) noexcept
{
    for (std::size_t i = 0; i < I::parameters.size(); ++i) {
        if (I::parameters[i].id == id) {
            return i;
        }
    }
    return invalidParameterIndex<I>;
}

template <class I>
double clampedParameterValue(std::size_t index, double value) noexcept
{
    return std::clamp(value, I::parameters[index].minimum, I::parameters[index].maximum);
}

void copyText(char* destination, std::size_t capacity, const char* source) noexcept
{
    if (capacity == 0) {
        return;
    }
    std::snprintf(destination, capacity, "%s", source);
}

template <std::size_t N>
void writeU32(std::array<std::uint8_t, N>& bytes, std::size_t& offset,
              std::uint32_t value) noexcept
{
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes[offset++] = static_cast<std::uint8_t>(value >> shift);
    }
}

template <std::size_t N>
std::uint32_t readU32(const std::array<std::uint8_t, N>& bytes,
                      std::size_t& offset) noexcept
{
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    }
    return value;
}

template <std::size_t N>
void writeDouble(std::array<std::uint8_t, N>& bytes, std::size_t& offset,
                 double value) noexcept
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes[offset++] = static_cast<std::uint8_t>(bits >> shift);
    }
}

template <std::size_t N>
double readDouble(const std::array<std::uint8_t, N>& bytes,
                  std::size_t& offset) noexcept
{
    std::uint64_t bits = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bits |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
    }
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool readExactly(const clap_istream_t* stream, void* destination, std::size_t size) noexcept
{
    if (stream == nullptr || stream->read == nullptr) {
        return false;
    }

    auto* bytes = static_cast<std::uint8_t*>(destination);
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = stream->read(stream, bytes + offset, size - offset);
        if (count <= 0 || static_cast<std::uint64_t>(count) > size - offset) {
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool writeExactly(const clap_ostream_t* stream, const void* source, std::size_t size) noexcept
{
    if (stream == nullptr || stream->write == nullptr) {
        return false;
    }

    const auto* bytes = static_cast<const std::uint8_t*>(source);
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = stream->write(stream, bytes + offset, size - offset);
        if (count <= 0 || static_cast<std::uint64_t>(count) > size - offset) {
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

template <class I>
class Adapter {
public:
    explicit Adapter(const clap_host_t* host) noexcept
        : host_(host)
    {
        plugin_.desc = &descriptor_;
        plugin_.plugin_data = this;
        plugin_.init = &init;
        plugin_.destroy = &destroy;
        plugin_.activate = &activate;
        plugin_.deactivate = &deactivate;
        plugin_.start_processing = &startProcessing;
        plugin_.stop_processing = &stopProcessing;
        plugin_.reset = &reset;
        plugin_.process = &process;
        plugin_.get_extension = &getExtension;
        plugin_.on_main_thread = &onMainThread;

        for (std::size_t i = 0; i < I::parameters.size(); ++i) {
            parameterValues_[i].store(I::parameters[i].defaultValue, std::memory_order_relaxed);
        }
    }

    const clap_plugin_t* plugin() const noexcept { return &plugin_; }
    static const clap_plugin_descriptor_t* descriptor() noexcept { return &descriptor_; }

private:
    static Adapter& self(const clap_plugin_t* plugin) noexcept
    {
        return *static_cast<Adapter*>(plugin->plugin_data);
    }

    static bool init(const clap_plugin_t*) noexcept { return true; }

    static void destroy(const clap_plugin_t* plugin) noexcept
    {
        auto& adapter = self(plugin);
        pianocraft::gui::destroy(adapter.editor_); // a host that skipped gui.destroy
        delete &adapter;
    }

    static bool activate(const clap_plugin_t* plugin, double sampleRate,
                         std::uint32_t, std::uint32_t) noexcept
    {
        auto& adapter = self(plugin);
        if (adapter.active_ || !std::isfinite(sampleRate) || sampleRate <= 0.0) {
            return false;
        }

        try {
            adapter.engine_.prepare(sampleRate);
            adapter.applyAllParameters();
            adapter.engine_.reset();
            adapter.active_ = true;
            return true;
        } catch (...) {
            adapter.active_ = false;
            adapter.processing_ = false;
            return false;
        }
    }

    static void deactivate(const clap_plugin_t* plugin) noexcept
    {
        auto& adapter = self(plugin);
        adapter.processing_ = false;
        adapter.active_ = false;
    }

    static bool startProcessing(const clap_plugin_t* plugin) noexcept
    {
        auto& adapter = self(plugin);
        adapter.processing_ = adapter.active_;
        return adapter.processing_;
    }

    static void stopProcessing(const clap_plugin_t* plugin) noexcept
    {
        self(plugin).processing_ = false;
    }

    static void reset(const clap_plugin_t* plugin) noexcept
    {
        auto& adapter = self(plugin);
        if (adapter.active_) {
            adapter.engine_.reset();
        }
    }

    void applyAllParameters() noexcept
    {
        for (std::size_t i = 0; i < I::parameters.size(); ++i) {
            engine_.setParameter(static_cast<typename I::Id>(I::parameters[i].id),
                                 parameterValues_[i].load(std::memory_order_relaxed));
        }
        appliedParameterRevision_ = parameterRevision_.load(std::memory_order_relaxed);
    }

    void synchronizeParameters() noexcept
    {
        const auto revision = parameterRevision_.load(std::memory_order_acquire);
        if (revision == appliedParameterRevision_) {
            return;
        }

        for (std::size_t i = 0; i < I::parameters.size(); ++i) {
            engine_.setParameter(static_cast<typename I::Id>(I::parameters[i].id),
                                 parameterValues_[i].load(std::memory_order_relaxed));
        }
        appliedParameterRevision_ = revision;
    }

    void setParameter(std::size_t index, double value, bool updateEngine) noexcept
    {
        if (!std::isfinite(value)) {
            return;
        }
        const double clamped = clampedParameterValue<I>(index, value);
        parameterValues_[index].store(clamped, std::memory_order_relaxed);
        parameterRevision_.fetch_add(1, std::memory_order_release);
        if (updateEngine) {
            engine_.setParameter(static_cast<typename I::Id>(I::parameters[index].id), clamped);
        }
    }

    // The editor's edits: applied, and reported to the host as gesture and value events.
    void applyEdits(const clap_output_events_t* out) noexcept
    {
        edits_.drain(
            out, [](std::uint32_t index) { return I::parameters[index].id; },
            [this](std::uint32_t index, double value) { setParameter(index, value, true); });
    }

    void clearOutputs(const clap_process_t* process) noexcept
    {
        if (process == nullptr || process->audio_outputs == nullptr || process->frames_count == 0) {
            return;
        }

        for (std::uint32_t port = 0; port < process->audio_outputs_count; ++port) {
            auto& output = process->audio_outputs[port];
            if (output.data32 != nullptr) {
                for (std::uint32_t channel = 0; channel < output.channel_count; ++channel) {
                    if (output.data32[channel] != nullptr) {
                        std::fill_n(output.data32[channel], process->frames_count, 0.0F);
                    }
                }
            } else if (output.data64 != nullptr) {
                for (std::uint32_t channel = 0; channel < output.channel_count; ++channel) {
                    if (output.data64[channel] != nullptr) {
                        std::fill_n(output.data64[channel], process->frames_count, 0.0);
                    }
                }
            }
        }
    }

    bool renderTargets(const clap_process_t* process, float*& left, float*& right) const noexcept
    {
        if (!active_ || !processing_ || process->audio_outputs_count == 0 ||
            process->audio_outputs == nullptr) {
            return false;
        }

        auto& output = process->audio_outputs[0];
        if (output.channel_count != 2 || output.data32 == nullptr || output.data32[0] == nullptr ||
            output.data32[1] == nullptr) {
            return false;
        }
        left = output.data32[0];
        right = output.data32[1];
        return true;
    }

    void renderSegment(float* left, float* right, std::uint32_t offset,
                       std::uint32_t count) noexcept
    {
        if (count != 0 && left != nullptr && right != nullptr) {
            engine_.render(left + offset, right + offset, count);
        }
    }

    static NoteAddress noteAddress(const clap_event_note_t& event) noexcept
    {
        return NoteAddress{event.note_id, event.port_index, event.channel, event.key};
    }

    void handleMidi(const clap_event_midi_t& event) noexcept
    {
        const auto status = event.data[0] & 0xF0U;
        const auto channel = static_cast<std::int16_t>(event.data[0] & 0x0FU);
        if (event.port_index > static_cast<std::uint16_t>(
                                   std::numeric_limits<std::int16_t>::max())) {
            return;
        }
        const auto port = static_cast<std::int16_t>(event.port_index);

        if (status == 0xB0U) {
            engine_.controlChange(channel, event.data[1], event.data[2] / 127.0);
            return;
        }
        if (status == 0xE0U) {
            engine_.pitchBend(channel, ((event.data[2] << 7 | event.data[1]) - 8192) / 8192.0);
            return;
        }

        if (status != 0x80U && status != 0x90U) {
            return;
        }

        const NoteAddress address{-1, port, channel, static_cast<std::int16_t>(event.data[1])};
        const double velocity = static_cast<double>(event.data[2]) / 127.0;
        if (status == 0x90U && event.data[2] != 0U) {
            engine_.noteOn(address, velocity);
        } else {
            engine_.noteOff(address, velocity);
        }
    }

    void handleEvent(const clap_event_header_t* header, bool parametersOnly = false) noexcept
    {
        if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID) {
            return;
        }

        if (header->type == CLAP_EVENT_PARAM_VALUE) {
            if (header->size < sizeof(clap_event_param_value_t)) {
                return;
            }
            const auto& event = *reinterpret_cast<const clap_event_param_value_t*>(header);
            const auto index = parameterIndex<I>(event.param_id);
            if (index != invalidParameterIndex<I>) {
                setParameter(index, event.value, true);
            }
            return;
        }

        if (header->type == CLAP_EVENT_PARAM_MOD) {
            if (header->size < sizeof(clap_event_param_mod_t)) {
                return;
            }
            const auto& event = *reinterpret_cast<const clap_event_param_mod_t*>(header);
            if (parameterIndex<I>(event.param_id) != invalidParameterIndex<I>) {
                engine_.modulate(NoteAddress{event.note_id, event.port_index, event.channel, event.key},
                                 static_cast<typename I::Id>(event.param_id), event.amount);
            }
            return;
        }

        if (parametersOnly) {
            return;
        }

        if (header->type == CLAP_EVENT_NOTE_EXPRESSION && header->size >= sizeof(clap_event_note_expression_t)) {
            const auto& event = *reinterpret_cast<const clap_event_note_expression_t*>(header);
            const NoteAddress target{event.note_id, event.port_index, event.channel, event.key};
            if (event.expression_id == CLAP_NOTE_EXPRESSION_VOLUME) {
                engine_.setNoteVolume(target, event.value);
            } else if (event.expression_id == CLAP_NOTE_EXPRESSION_PAN) {
                engine_.setNotePan(target, event.value);
            } else if (event.expression_id == CLAP_NOTE_EXPRESSION_BRIGHTNESS) {
                engine_.setNoteBrightness(target, event.value);
            } else if (event.expression_id == CLAP_NOTE_EXPRESSION_PRESSURE) {
                engine_.setNotePressure(target, event.value);
            }
            return;
        }

        if ((header->type == CLAP_EVENT_NOTE_ON || header->type == CLAP_EVENT_NOTE_OFF ||
             header->type == CLAP_EVENT_NOTE_CHOKE) &&
            header->size >= sizeof(clap_event_note_t)) {
            const auto& event = *reinterpret_cast<const clap_event_note_t*>(header);
            if (header->type == CLAP_EVENT_NOTE_CHOKE) {
                engine_.choke(noteAddress(event));
            } else if (!std::isfinite(event.velocity)) {
                return;
            } else if (header->type == CLAP_EVENT_NOTE_ON) {
                if (event.port_index < 0 || event.channel < 0 || event.key < 0) {
                    return;
                }
                engine_.noteOn(noteAddress(event), std::clamp(event.velocity, 0.0, 1.0));
            } else {
                engine_.noteOff(noteAddress(event), std::clamp(event.velocity, 0.0, 1.0));
            }
            return;
        }

        if (header->type == CLAP_EVENT_MIDI && header->size >= sizeof(clap_event_midi_t)) {
            handleMidi(*reinterpret_cast<const clap_event_midi_t*>(header));
        }
    }

    static clap_process_status process(const clap_plugin_t* plugin,
                                       const clap_process_t* process) noexcept
    {
        if (process == nullptr) {
            return CLAP_PROCESS_ERROR;
        }

        auto& adapter = self(plugin);
        adapter.clearOutputs(process);
        float* left = nullptr;
        float* right = nullptr;
        adapter.renderTargets(process, left, right);
        adapter.synchronizeParameters();
        adapter.applyEdits(process->out_events);

        std::uint32_t offset = 0;
        const auto eventCount = (process->in_events != nullptr && process->in_events->size != nullptr &&
                                 process->in_events->get != nullptr)
                                    ? process->in_events->size(process->in_events)
                                    : 0;
        for (std::uint32_t i = 0; i < eventCount; ++i) {
            const auto* header = process->in_events->get(process->in_events, i);
            if (header == nullptr || header->time > process->frames_count) {
                continue;
            }
            const auto eventOffset = std::max(offset, header->time);
            adapter.renderSegment(left, right, offset, eventOffset - offset);
            adapter.reportEndedNotes(process->out_events, offset);
            offset = eventOffset;
            adapter.handleEvent(header);
            adapter.reportEndedNotes(process->out_events, offset);
        }
        adapter.renderSegment(left, right, offset, process->frames_count - offset);
        adapter.reportEndedNotes(process->out_events, offset);
        return CLAP_PROCESS_CONTINUE;
    }

    static std::uint32_t audioPortsCount(const clap_plugin_t*, bool isInput) noexcept
    {
        return isInput ? 0U : 1U;
    }

    static bool audioPortsGet(const clap_plugin_t*, std::uint32_t index, bool isInput,
                              clap_audio_port_info_t* info) noexcept
    {
        if (isInput || index != 0 || info == nullptr) {
            return false;
        }
        std::memset(info, 0, sizeof(*info));
        info->id = 0;
        copyText(info->name, sizeof(info->name), "Main Output");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    }

    static std::uint32_t notePortsCount(const clap_plugin_t*, bool isInput) noexcept
    {
        return isInput ? 1U : 0U;
    }

    static bool notePortsGet(const clap_plugin_t*, std::uint32_t index, bool isInput,
                             clap_note_port_info_t* info) noexcept
    {
        if (!isInput || index != 0 || info == nullptr) {
            return false;
        }
        std::memset(info, 0, sizeof(*info));
        info->id = 0;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        copyText(info->name, sizeof(info->name), "Note Input");
        return true;
    }

    static std::uint32_t paramsCount(const clap_plugin_t*) noexcept
    {
        return static_cast<std::uint32_t>(I::parameters.size());
    }

    static bool paramsGetInfo(const clap_plugin_t*, std::uint32_t index,
                              clap_param_info_t* info) noexcept
    {
        if (index >= I::parameters.size() || info == nullptr) {
            return false;
        }
        const auto& spec = I::parameters[index];
        std::memset(info, 0, sizeof(*info));
        info->id = spec.id;
        info->flags = spec.flags;
        info->cookie = nullptr;
        copyText(info->name, sizeof(info->name), spec.name);
        copyText(info->module, sizeof(info->module), spec.module);
        info->min_value = spec.minimum;
        info->max_value = spec.maximum;
        info->default_value = spec.defaultValue;
        return true;
    }

    static bool paramsGetValue(const clap_plugin_t* plugin, clap_id id,
                               double* value) noexcept
    {
        const auto index = parameterIndex<I>(id);
        if (index == invalidParameterIndex<I> || value == nullptr) {
            return false;
        }
        *value = self(plugin).parameterValues_[index].load(std::memory_order_acquire);
        return true;
    }

    static bool paramsValueToText(const clap_plugin_t*, clap_id id, double value,
                                  char* output, std::uint32_t capacity) noexcept
    {
        const auto index = parameterIndex<I>(id);
        if (index == invalidParameterIndex<I> || output == nullptr || capacity == 0 ||
            !std::isfinite(value)) {
            return false;
        }
        const double clamped = clampedParameterValue<I>(index, value);
        const auto* choices = I::parameters[index].choices;
        const auto result = choices != nullptr
            ? std::snprintf(output, capacity, "%s", choices[static_cast<std::size_t>(std::lround(clamped))])
            : std::snprintf(output, capacity, "%.6g", clamped);
        return result >= 0 && static_cast<std::uint32_t>(result) < capacity;
    }

    static bool paramsTextToValue(const clap_plugin_t*, clap_id id, const char* text,
                                  double* value) noexcept
    {
        const auto index = parameterIndex<I>(id);
        if (index == invalidParameterIndex<I> || text == nullptr || value == nullptr) {
            return false;
        }
        if (const auto* choices = I::parameters[index].choices; choices != nullptr) {
            for (std::size_t i = 0; i <= static_cast<std::size_t>(I::parameters[index].maximum); ++i) {
                if (std::strcmp(text, choices[i]) == 0) {
                    *value = static_cast<double>(i);
                    return true;
                }
            }
        }

        errno = 0;
        char* end = nullptr;
        const double parsed = std::strtod(text, &end);
        if (end == text || errno == ERANGE || !std::isfinite(parsed)) {
            return false;
        }
        while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r' || *end == '\f' ||
               *end == '\v') {
            ++end;
        }
        if (*end != '\0') {
            return false;
        }
        *value = clampedParameterValue<I>(index, parsed);
        return true;
    }

    static void paramsFlush(const clap_plugin_t* plugin, const clap_input_events_t* input,
                            const clap_output_events_t* output) noexcept
    {
        auto& adapter = self(plugin);
        adapter.synchronizeParameters();
        adapter.applyEdits(output);
        if (input == nullptr || input->size == nullptr || input->get == nullptr) {
            return;
        }
        const auto count = input->size(input);
        for (std::uint32_t i = 0; i < count; ++i) {
            adapter.handleEvent(input->get(input, i), true);
        }
    }

    static bool stateSave(const clap_plugin_t* plugin, const clap_ostream_t* stream) noexcept
    {
        const auto& adapter = self(plugin);
        std::array<std::uint8_t, stateSize<I>> bytes{};
        std::size_t offset = 0;
        bytes[offset++] = static_cast<std::uint8_t>('P');
        bytes[offset++] = static_cast<std::uint8_t>('C');
        bytes[offset++] = static_cast<std::uint8_t>('S');
        bytes[offset++] = static_cast<std::uint8_t>('T');
        writeU32(bytes, offset, stateVersion);
        writeU32(bytes, offset, static_cast<std::uint32_t>(I::parameters.size()));
        for (std::size_t i = 0; i < I::parameters.size(); ++i) {
            writeU32(bytes, offset, I::parameters[i].id);
            writeDouble(bytes, offset, adapter.parameterValues_[i].load(std::memory_order_acquire));
        }
        return writeExactly(stream, bytes.data(), bytes.size());
    }

    static bool stateLoad(const clap_plugin_t* plugin, const clap_istream_t* stream) noexcept
    {
        auto& adapter = self(plugin);
        // States from older versions carry fewer parameters; the missing ones keep defaults.
        std::array<std::uint8_t, stateSize<I>> bytes{};
        constexpr std::size_t headerSize = 12;
        constexpr std::size_t recordSize = 12;
        if (!readExactly(stream, bytes.data(), headerSize)) {
            return false;
        }

        std::size_t offset = 0;
        if (bytes[offset++] != static_cast<std::uint8_t>('P') ||
            bytes[offset++] != static_cast<std::uint8_t>('C') ||
            bytes[offset++] != static_cast<std::uint8_t>('S') ||
            bytes[offset++] != static_cast<std::uint8_t>('T') ||
            readU32(bytes, offset) != stateVersion) {
            return false;
        }
        const std::uint32_t recordCount = readU32(bytes, offset);
        if (recordCount == 0 || recordCount > I::parameters.size() ||
            !readExactly(stream, bytes.data() + headerSize, recordCount * recordSize)) {
            return false;
        }

        std::array<double, I::parameters.size()> loadedValues{};
        for (std::size_t i = 0; i < I::parameters.size(); ++i) {
            loadedValues[i] = I::parameters[i].defaultValue;
        }
        std::array<bool, I::parameters.size()> seen{};
        for (std::size_t record = 0; record < recordCount; ++record) {
            const auto index = parameterIndex<I>(readU32(bytes, offset));
            const double value = readDouble(bytes, offset);
            if (index == invalidParameterIndex<I> || seen[index] || !std::isfinite(value) ||
                value < I::parameters[index].minimum || value > I::parameters[index].maximum) {
                return false;
            }
            seen[index] = true;
            loadedValues[index] = value;
        }

        std::uint8_t trailingByte = 0;
        if (stream->read(stream, &trailingByte, 1) != 0) {
            return false;
        }
        adapter.adopt(loadedValues);
        return true;
    }

    // [main-thread] Takes a full set of parameter values (state or preset) and tells the host.
    void adopt(const std::array<double, I::parameters.size()>& values) noexcept
    {
        for (std::size_t i = 0; i < I::parameters.size(); ++i) {
            parameterValues_[i].store(values[i], std::memory_order_relaxed);
        }
        parameterRevision_.fetch_add(1, std::memory_order_release);
        if (!active_) {
            applyAllParameters();
        }
        if (host_ != nullptr && host_->get_extension != nullptr) {
            const auto* hostParams = static_cast<const clap_host_params_t*>(host_->get_extension(host_, CLAP_EXT_PARAMS));
            if (hostParams != nullptr && hostParams->rescan != nullptr) {
                hostParams->rescan(host_, CLAP_PARAM_RESCAN_VALUES);
            }
        }
    }

    static bool presetLoad(const clap_plugin_t* plugin, std::uint32_t locationKind, const char* location,
                           const char* loadKey) noexcept
    {
        auto& adapter = self(plugin);
        const Preset* preset = findPreset<I>(locationKind, location);
        const clap_host_preset_load_t* hostPresets = nullptr;
        if (adapter.host_ != nullptr && adapter.host_->get_extension != nullptr) {
            hostPresets = static_cast<const clap_host_preset_load_t*>(
                adapter.host_->get_extension(adapter.host_, CLAP_EXT_PRESET_LOAD));
        }
        if (preset == nullptr) {
            if (hostPresets != nullptr && hostPresets->on_error != nullptr) {
                hostPresets->on_error(adapter.host_, locationKind, location, loadKey, 0, "Unknown PianoCraft preset");
            }
            return false;
        }
        std::array<double, I::parameters.size()> values{};
        for (std::size_t i = 0; i < I::parameters.size(); ++i) {
            values[i] = I::parameters[i].defaultValue;
        }
        for (const auto& [id, value] : preset->changes) {
            if (id != 0) {
                values[parameterIndex<I>(id)] = value;
            }
        }
        adapter.adopt(values);
        if (hostPresets != nullptr && hostPresets->loaded != nullptr) {
            hostPresets->loaded(adapter.host_, locationKind, location, loadKey);
        }
        return true;
    }

    // NOTE_END tells the host when a voice really stops (a pedalled note outlives its note-off), so
    // its per-voice modulators live exactly as long.
    void reportEndedNotes(const clap_output_events_t* out, std::uint32_t offset) noexcept
    {
        std::array<pianocraft::EndedNote, I::Engine::voiceCapacity> ended{};
        const auto count = engine_.takeEndedNotes(ended.data(), ended.size());
        if (out == nullptr || out->try_push == nullptr) {
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            clap_event_note_t event{};
            event.header.size = sizeof(event);
            event.header.time = offset + ended[i].frame;
            event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            event.header.type = CLAP_EVENT_NOTE_END;
            event.note_id = ended[i].address.noteId;
            event.port_index = ended[i].address.port;
            event.channel = ended[i].address.channel;
            event.key = ended[i].address.key;
            out->try_push(out, &event.header);
        }
    }

    static bool voiceInfoGet(const clap_plugin_t*, clap_voice_info_t* info) noexcept
    {
        if (info == nullptr) {
            return false;
        }
        info->voice_count = static_cast<std::uint32_t>(I::Engine::voiceCapacity);
        info->voice_capacity = info->voice_count;
        info->flags = CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES;
        return true;
    }

    static std::uint32_t remoteControlsCount(const clap_plugin_t*) noexcept
    {
        return static_cast<std::uint32_t>(I::pages.size());
    }

    static bool remoteControlsGet(const clap_plugin_t*, std::uint32_t index,
                                  clap_remote_controls_page_t* page) noexcept
    {
        if (index >= I::pages.size() || page == nullptr) {
            return false;
        }
        std::memset(page, 0, sizeof(*page));
        copyText(page->section_name, sizeof(page->section_name), I::section);
        page->page_id = index;
        copyText(page->page_name, sizeof(page->page_name), I::pages[index].name);
        std::copy(I::pages[index].parameters.begin(), I::pages[index].parameters.end(), page->param_ids);
        return true;
    }

    static const void* getExtension(const clap_plugin_t* plugin, const char* id) noexcept
    {
        if (id == nullptr) {
            return nullptr;
        }
        if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) {
            return &audioPorts_;
        }
        if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) {
            return &notePorts_;
        }
        if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) {
            return &params_;
        }
        if (std::strcmp(id, CLAP_EXT_STATE) == 0) {
            return &state_;
        }
        if (std::strcmp(id, CLAP_EXT_REMOTE_CONTROLS) == 0 || std::strcmp(id, CLAP_EXT_REMOTE_CONTROLS_COMPAT) == 0) {
            return &remoteControls_;
        }
        if (std::strcmp(id, CLAP_EXT_PRESET_LOAD) == 0 || std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT) == 0) {
            return &presetLoad_;
        }
        if (std::strcmp(id, CLAP_EXT_VOICE_INFO) == 0) {
            return &voiceInfo_;
        }
        if (std::strcmp(id, CLAP_EXT_GUI) == 0) {
            return &gui_;
        }
        (void)plugin;
        return nullptr;
    }

    static void onMainThread(const clap_plugin_t*) noexcept {}

    // ---- gui: the generic AppKit editor (ClapGui.mm), embedded or floating, Cocoa only --------

    static bool guiIsApiSupported(const clap_plugin_t*, const char* api, bool) noexcept
    {
        return api != nullptr && std::strcmp(api, CLAP_WINDOW_API_COCOA) == 0;
    }

    static bool guiGetPreferredApi(const clap_plugin_t*, const char** api, bool* isFloating) noexcept
    {
        if (api == nullptr || isFloating == nullptr) {
            return false;
        }
        *api = CLAP_WINDOW_API_COCOA;
        *isFloating = false;
        return true;
    }

    static bool guiCreate(const clap_plugin_t* plugin, const char* api, bool isFloating) noexcept
    {
        auto& adapter = self(plugin);
        if (adapter.editor_ != nullptr || !guiIsApiSupported(plugin, api, isFloating)) {
            return false;
        }
        std::array<pianocraft::gui::Param, I::parameters.size()> params{};
        for (std::size_t i = 0; i < params.size(); ++i) {
            const auto& spec = I::parameters[i];
            params[i] = {spec.name, spec.module, spec.minimum, spec.maximum, (spec.flags & CLAP_PARAM_IS_STEPPED) != 0,
                         spec.choices};
        }
        const pianocraft::gui::Client client{&adapter, &guiValue, &guiEdit, &guiText, &guiClosed};
        adapter.editor_ = pianocraft::gui::create(params.data(), params.size(), client, isFloating, I::name);
        return adapter.editor_ != nullptr;
    }

    static void guiDestroy(const clap_plugin_t* plugin) noexcept
    {
        auto& adapter = self(plugin);
        pianocraft::gui::destroy(adapter.editor_);
        adapter.editor_ = nullptr;
    }

    // Cocoa sizes are logical: gui.h says not to call set_scale for it.
    static bool guiSetScale(const clap_plugin_t*, double) noexcept { return false; }

    static bool guiGetSize(const clap_plugin_t* plugin, std::uint32_t* width, std::uint32_t* height) noexcept
    {
        const auto& adapter = self(plugin);
        if (adapter.editor_ == nullptr || width == nullptr || height == nullptr) {
            return false;
        }
        pianocraft::gui::size(adapter.editor_, width, height);
        return true;
    }

    static bool guiCanResize(const clap_plugin_t*) noexcept { return false; }
    static bool guiGetResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t*) noexcept { return false; }
    static bool guiAdjustSize(const clap_plugin_t*, std::uint32_t*, std::uint32_t*) noexcept { return false; }
    static bool guiSetSize(const clap_plugin_t*, std::uint32_t, std::uint32_t) noexcept { return false; }

    static bool guiSetParent(const clap_plugin_t* plugin, const clap_window_t* window) noexcept
    {
        auto& adapter = self(plugin);
        if (adapter.editor_ == nullptr || window == nullptr || window->api == nullptr ||
            std::strcmp(window->api, CLAP_WINDOW_API_COCOA) != 0) {
            return false;
        }
        return pianocraft::gui::setParent(adapter.editor_, window->cocoa);
    }

    static bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) noexcept { return false; }

    static void guiSuggestTitle(const clap_plugin_t* plugin, const char* title) noexcept
    {
        if (auto* editor = self(plugin).editor_; editor != nullptr) {
            pianocraft::gui::setTitle(editor, title);
        }
    }

    static bool guiShow(const clap_plugin_t* plugin) noexcept
    {
        auto* editor = self(plugin).editor_;
        return editor != nullptr && pianocraft::gui::show(editor);
    }

    static bool guiHide(const clap_plugin_t* plugin) noexcept
    {
        auto* editor = self(plugin).editor_;
        return editor != nullptr && pianocraft::gui::hide(editor);
    }

    // The editor's callbacks (main thread).
    static double guiValue(void* context, std::size_t index) noexcept
    {
        return static_cast<Adapter*>(context)->parameterValues_[index].load(std::memory_order_acquire);
    }

    static void guiEdit(void* context, std::size_t index, double value, pianocraft::EditPhase phase) noexcept
    {
        auto& adapter = *static_cast<Adapter*>(context);
        if (!adapter.edits_.push({static_cast<std::uint32_t>(index), value, phase})) {
            return;
        }
        // While the plugin is not processing, the host flushes; while it is, this is harmless.
        if (adapter.host_ != nullptr && adapter.host_->get_extension != nullptr) {
            const auto* hostParams = static_cast<const clap_host_params_t*>(adapter.host_->get_extension(adapter.host_, CLAP_EXT_PARAMS));
            if (hostParams != nullptr && hostParams->request_flush != nullptr) {
                hostParams->request_flush(adapter.host_);
            }
        }
    }

    static bool guiText(void* context, std::size_t index, double value, char* out, std::uint32_t capacity) noexcept
    {
        auto& adapter = *static_cast<Adapter*>(context);
        return paramsValueToText(&adapter.plugin_, I::parameters[index].id, value, out, capacity);
    }

    static void guiClosed(void* context) noexcept
    {
        auto& adapter = *static_cast<Adapter*>(context);
        if (adapter.host_ != nullptr && adapter.host_->get_extension != nullptr) {
            const auto* hostGui = static_cast<const clap_host_gui_t*>(adapter.host_->get_extension(adapter.host_, CLAP_EXT_GUI));
            if (hostGui != nullptr && hostGui->closed != nullptr) {
                hostGui->closed(adapter.host_, false);
            }
        }
    }

    const clap_host_t* host_;
    typename I::Engine engine_;
    clap_plugin_t plugin_{};
    std::array<std::atomic<double>, I::parameters.size()> parameterValues_{};
    std::atomic<std::uint32_t> parameterRevision_{1};
    std::uint32_t appliedParameterRevision_ = 0;
    bool active_ = false;
    bool processing_ = false;
    pianocraft::GuiEdits edits_;
    pianocraft::gui::Editor* editor_ = nullptr;

    static const clap_plugin_descriptor_t descriptor_;
    static const clap_plugin_audio_ports_t audioPorts_;
    static const clap_plugin_note_ports_t notePorts_;
    static const clap_plugin_params_t params_;
    static const clap_plugin_state_t state_;
    static const clap_plugin_remote_controls_t remoteControls_;
    static const clap_plugin_voice_info_t voiceInfo_;
    static const clap_plugin_preset_load_t presetLoad_;
    static const clap_plugin_gui_t gui_;
};

template <class I>
const clap_plugin_descriptor_t Adapter<I>::descriptor_ = {
    CLAP_VERSION_INIT, I::id, I::name, "PianoCraft", "", "", "", "0.1.0", I::description, pluginFeatures,
};

template <class I>
const clap_plugin_gui_t Adapter<I>::gui_ = {
    &Adapter<I>::guiIsApiSupported, &Adapter<I>::guiGetPreferredApi, &Adapter<I>::guiCreate,
    &Adapter<I>::guiDestroy,        &Adapter<I>::guiSetScale,        &Adapter<I>::guiGetSize,
    &Adapter<I>::guiCanResize,      &Adapter<I>::guiGetResizeHints,  &Adapter<I>::guiAdjustSize,
    &Adapter<I>::guiSetSize,        &Adapter<I>::guiSetParent,       &Adapter<I>::guiSetTransient,
    &Adapter<I>::guiSuggestTitle,   &Adapter<I>::guiShow,            &Adapter<I>::guiHide,
};

template <class I>
const clap_plugin_audio_ports_t Adapter<I>::audioPorts_ = {
    &Adapter<I>::audioPortsCount,
    &Adapter<I>::audioPortsGet,
};

template <class I>
const clap_plugin_note_ports_t Adapter<I>::notePorts_ = {
    &Adapter<I>::notePortsCount,
    &Adapter<I>::notePortsGet,
};

template <class I>
const clap_plugin_params_t Adapter<I>::params_ = {
    &Adapter<I>::paramsCount,
    &Adapter<I>::paramsGetInfo,
    &Adapter<I>::paramsGetValue,
    &Adapter<I>::paramsValueToText,
    &Adapter<I>::paramsTextToValue,
    &Adapter<I>::paramsFlush,
};

template <class I>
const clap_plugin_state_t Adapter<I>::state_ = {
    &Adapter<I>::stateSave,
    &Adapter<I>::stateLoad,
};

template <class I>
const clap_plugin_remote_controls_t Adapter<I>::remoteControls_ = {
    &Adapter<I>::remoteControlsCount,
    &Adapter<I>::remoteControlsGet,
};

template <class I>
const clap_plugin_voice_info_t Adapter<I>::voiceInfo_ = {
    &Adapter<I>::voiceInfoGet,
};

template <class I>
const clap_plugin_preset_load_t Adapter<I>::presetLoad_ = {
    &Adapter<I>::presetLoad,
};

const std::array<const clap_plugin_descriptor_t*, 12> descriptors{
    Adapter<Piano>::descriptor(), Adapter<Harp>::descriptor(), Adapter<Flute>::descriptor(),
    Adapter<Strings>::descriptor(), Adapter<Horn>::descriptor(), Adapter<Trumpet>::descriptor(),
    Adapter<Trombone>::descriptor(), Adapter<Tuba>::descriptor(), Adapter<Oboe>::descriptor(),
    Adapter<Clarinet>::descriptor(), Adapter<Celesta>::descriptor(), Adapter<Crotales>::descriptor()};

std::uint32_t pluginCount(const clap_plugin_factory_t*) noexcept
{
    return static_cast<std::uint32_t>(descriptors.size());
}

const clap_plugin_descriptor_t* pluginDescriptor(const clap_plugin_factory_t*,
                                                 std::uint32_t index) noexcept
{
    return index < descriptors.size() ? descriptors[index] : nullptr;
}

template <class I>
const clap_plugin_t* create(const clap_host_t* host) noexcept
{
    try {
        return (new Adapter<I>(host))->plugin();
    } catch (...) {
        return nullptr;
    }
}

const clap_plugin_t* createPlugin(const clap_plugin_factory_t*, const clap_host_t* host,
                                  const char* pluginId) noexcept
{
    if (host == nullptr || pluginId == nullptr || !clap_version_is_compatible(host->clap_version)) {
        return nullptr;
    }
    if (std::strcmp(pluginId, Piano::id) == 0) {
        return create<Piano>(host);
    }
    if (std::strcmp(pluginId, Harp::id) == 0) {
        return create<Harp>(host);
    }
    if (std::strcmp(pluginId, Celesta::id) == 0) {
        return create<Celesta>(host);
    }
    if (std::strcmp(pluginId, Crotales::id) == 0) {
        return create<Crotales>(host);
    }
    if (std::strcmp(pluginId, Flute::id) == 0) {
        return create<Flute>(host);
    }
    if (std::strcmp(pluginId, Strings::id) == 0) {
        return create<Strings>(host);
    }
    if (std::strcmp(pluginId, Horn::id) == 0) {
        return create<Horn>(host);
    }
    if (std::strcmp(pluginId, Trumpet::id) == 0) {
        return create<Trumpet>(host);
    }
    if (std::strcmp(pluginId, Trombone::id) == 0) {
        return create<Trombone>(host);
    }
    if (std::strcmp(pluginId, Tuba::id) == 0) {
        return create<Tuba>(host);
    }
    if (std::strcmp(pluginId, Oboe::id) == 0) {
        return create<Oboe>(host);
    }
    if (std::strcmp(pluginId, Clarinet::id) == 0) {
        return create<Clarinet>(host);
    }
    return nullptr;
}

const clap_plugin_factory_t pluginFactory = {
    &pluginCount,
    &pluginDescriptor,
    &createPlugin,
};

// Preset discovery: one provider, one folder of marker files in the bundle, one preset per file.
const clap_preset_discovery_provider_descriptor_t presetProviderDescriptor = {
    CLAP_VERSION_INIT,
    "com.pianocraft.upright.presets",
    "PianoCraft presets",
    "PianoCraft",
};

bool presetProviderInit(const clap_preset_discovery_provider_t* provider) noexcept
{
    const auto* indexer = static_cast<const clap_preset_discovery_indexer_t*>(provider->provider_data);
    const clap_preset_discovery_filetype_t filetype{"PianoCraft preset", "A built-in PianoCraft voicing", "pianocraft"};
    const clap_preset_discovery_location_t location{CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "PianoCraft",
                                                    CLAP_PRESET_DISCOVERY_LOCATION_FILE, presetDirectory.c_str()};
    return indexer->declare_filetype(indexer, &filetype) && indexer->declare_location(indexer, &location);
}

void presetProviderDestroy(const clap_preset_discovery_provider_t* provider) noexcept
{
    delete provider;
}

template <class I>
bool describePreset(std::uint32_t locationKind, const char* location,
                    const clap_preset_discovery_metadata_receiver_t* receiver) noexcept
{
    const Preset* preset = findPreset<I>(locationKind, location);
    if (preset == nullptr) {
        return false;
    }
    const clap_universal_plugin_id_t pluginId{"clap", I::id};
    if (receiver->begin_preset(receiver, preset->name, nullptr)) {
        receiver->add_plugin_id(receiver, &pluginId);
        receiver->set_flags(receiver, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
        receiver->set_description(receiver, preset->description);
    }
    return true;
}

bool presetProviderGetMetadata(const clap_preset_discovery_provider_t*, std::uint32_t locationKind,
                               const char* location, const clap_preset_discovery_metadata_receiver_t* receiver) noexcept
{
    return receiver != nullptr && (describePreset<Piano>(locationKind, location, receiver) ||
                                   describePreset<Harp>(locationKind, location, receiver) ||
                                   describePreset<Flute>(locationKind, location, receiver) ||
                                   describePreset<Strings>(locationKind, location, receiver) ||
                                   describePreset<Horn>(locationKind, location, receiver) ||
                                   describePreset<Trumpet>(locationKind, location, receiver) ||
                                   describePreset<Trombone>(locationKind, location, receiver) ||
                                   describePreset<Tuba>(locationKind, location, receiver) ||
                                   describePreset<Oboe>(locationKind, location, receiver) ||
                                   describePreset<Clarinet>(locationKind, location, receiver) ||
                                   describePreset<Celesta>(locationKind, location, receiver) ||
                                   describePreset<Crotales>(locationKind, location, receiver));
}

const void* presetProviderGetExtension(const clap_preset_discovery_provider_t*, const char*) noexcept
{
    return nullptr;
}

std::uint32_t presetProviderCount(const clap_preset_discovery_factory_t*) noexcept
{
    return 1;
}

const clap_preset_discovery_provider_descriptor_t* presetProviderGetDescriptor(
    const clap_preset_discovery_factory_t*, std::uint32_t index) noexcept
{
    return index == 0 ? &presetProviderDescriptor : nullptr;
}

const clap_preset_discovery_provider_t* presetProviderCreate(const clap_preset_discovery_factory_t*,
                                                             const clap_preset_discovery_indexer_t* indexer,
                                                             const char* providerId) noexcept
{
    if (indexer == nullptr || providerId == nullptr || std::strcmp(providerId, presetProviderDescriptor.id) != 0) {
        return nullptr;
    }
    return new (std::nothrow) clap_preset_discovery_provider_t{
        &presetProviderDescriptor, const_cast<clap_preset_discovery_indexer_t*>(indexer), &presetProviderInit,
        &presetProviderDestroy, &presetProviderGetMetadata, &presetProviderGetExtension};
}

const clap_preset_discovery_factory_t presetDiscoveryFactory = {
    &presetProviderCount,
    &presetProviderGetDescriptor,
    &presetProviderCreate,
};

bool entryInit(const char* pluginPath) noexcept
{
    try {
        presetDirectory = std::string(pluginPath != nullptr ? pluginPath : "") + "/Contents/Resources/Presets";
    } catch (...) {
        return false;
    }
    return true;
}

void entryDeinit() noexcept {}

const void* getFactory(const char* factoryId) noexcept
{
    if (factoryId != nullptr && std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0) {
        return &pluginFactory;
    }
    if (factoryId != nullptr && (std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID) == 0 ||
                                 std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT) == 0)) {
        return &presetDiscoveryFactory;
    }
    return nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    &entryInit,
    &entryDeinit,
    &getFactory,
};
