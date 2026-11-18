// Loads the built CLAP binary and checks state compatibility (states saved before the mic
// parameters existed must still load, malformed state must be rejected), the advertised
// extensions, and that a finished voice reaches the host as NOTE_END. The other instruments get the
// same round trip, presets and NOTE_END checks.
#include <clap/clap.h>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#define dlopen(path, flags) LoadLibraryA(path)
#define dlsym(lib, name) reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name))
#define dlerror() "LoadLibrary failed"
#else
#include <dlfcn.h>
#endif
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "../src/host/GuiEdits.hpp"

// Records every event a plugin reports (the GUI edit check reads gestures and values).
struct Recorded { std::vector<uint16_t> types; std::vector<clap_id> ids; std::vector<double> values; };
static bool recordPush(const clap_output_events_t* list, const clap_event_header_t* event) {
    auto* r = static_cast<Recorded*>(list->ctx); r->types.push_back(event->type);
    if (event->type == CLAP_EVENT_PARAM_VALUE) {
        auto* v = reinterpret_cast<const clap_event_param_value_t*>(event); r->ids.push_back(v->param_id); r->values.push_back(v->value);
    } else if (event->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || event->type == CLAP_EVENT_PARAM_GESTURE_END) {
        r->ids.push_back(reinterpret_cast<const clap_event_param_gesture_t*>(event)->param_id);
    }
    return true;
}

struct Buffer { std::vector<uint8_t> data; size_t pos = 0; };
static int64_t readFn(const clap_istream_t* s, void* out, uint64_t size) {
    auto* b = static_cast<Buffer*>(s->ctx); size_t n = std::min<size_t>(size, b->data.size() - b->pos);
    std::memcpy(out, b->data.data() + b->pos, n); b->pos += n; return (int64_t)n;
}
static int64_t writeFn(const clap_ostream_t* s, const void* in, uint64_t size) {
    auto* b = static_cast<Buffer*>(s->ctx); auto* p = static_cast<const uint8_t*>(in); b->data.insert(b->data.end(), p, p + size); return (int64_t)size;
}
static const void* hostExt(const clap_host_t*, const char*) { return nullptr; }
struct Events { std::vector<clap_event_note_t> notes; };
static uint32_t eventsSize(const clap_input_events_t* list) { return (uint32_t)static_cast<Events*>(list->ctx)->notes.size(); }
static const clap_event_header_t* eventsGet(const clap_input_events_t* list, uint32_t i) { return &static_cast<Events*>(list->ctx)->notes[i].header; }
static bool eventsPush(const clap_output_events_t* list, const clap_event_header_t* event) {
    if (event->type == CLAP_EVENT_NOTE_END) static_cast<Events*>(list->ctx)->notes.push_back(*reinterpret_cast<const clap_event_note_t*>(event));
    return true;
}
static clap_event_note_t noteEvent(uint16_t type, uint32_t time, int32_t id, int16_t key = 60) {
    clap_event_note_t e{}; e.header = {sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, type, 0};
    e.note_id = id; e.port_index = 0; e.channel = 0; e.key = key; e.velocity = 0.6; return e;
}
static void noop(const clap_host_t*) {}
static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xff); }
static void putD(std::vector<uint8_t>& v, double d) { uint64_t b; std::memcpy(&b, &d, 8); for (int i = 0; i < 8; ++i) v.push_back((b >> (8 * i)) & 0xff); }
static std::vector<uint8_t> blob(std::vector<std::pair<uint32_t,double>> recs, bool trailing = false) {
    std::vector<uint8_t> v{'P','C','S','T'}; put32(v, 1); put32(v, (uint32_t)recs.size());
    for (auto& [id, val] : recs) { put32(v, id); putD(v, val); }
    if (trailing) v.push_back(0); return v;
}
// Preset discovery: a stand-in indexer and metadata receiver record what the provider declares.
struct Discovered { int locations = 0; uint32_t kind = 99; std::string path; std::vector<std::string> names; bool keyed = false; std::vector<std::string> pluginIds; };
static Discovered discovered;
static bool declareFiletype(const clap_preset_discovery_indexer_t*, const clap_preset_discovery_filetype_t*) { return true; }
static bool declareLocation(const clap_preset_discovery_indexer_t*, const clap_preset_discovery_location_t* l) {
    ++discovered.locations; discovered.kind = l->kind; discovered.path = l->location ? l->location : ""; return true;
}
static bool declareSoundpack(const clap_preset_discovery_indexer_t*, const clap_preset_discovery_soundpack_t*) { return true; }
static const void* indexerExtension(const clap_preset_discovery_indexer_t*, const char*) { return nullptr; }
static void onError(const clap_preset_discovery_metadata_receiver_t*, int32_t, const char*) {}
static bool beginPreset(const clap_preset_discovery_metadata_receiver_t*, const char* name, const char* key) {
    discovered.names.push_back(name); discovered.keyed = discovered.keyed || key != nullptr; return true;
}
static void addPluginId(const clap_preset_discovery_metadata_receiver_t*, const clap_universal_plugin_id_t* id) {
    discovered.pluginIds.push_back(std::strcmp(id->abi, "clap") == 0 ? id->id : "");
}
static void setText(const clap_preset_discovery_metadata_receiver_t*, const char*) {}
static void setFlags(const clap_preset_discovery_metadata_receiver_t*, uint32_t) {}
static void setTimestamps(const clap_preset_discovery_metadata_receiver_t*, clap_timestamp, clap_timestamp) {}
static void addExtraInfo(const clap_preset_discovery_metadata_receiver_t*, const char*, const char*) {}

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: pianocraft-state-tests <plugin binary> <plugin bundle>\n"); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW); if (!lib) { std::printf("dlopen failed: %s\n", dlerror()); return 1; }
    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry"));
    entry->init(argv[2]);
    auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    clap_host_t host{CLAP_VERSION, nullptr, "harness", "", "", "1", hostExt, noop, noop, noop};
    auto* plugin = factory->create_plugin(factory, &host, factory->get_plugin_descriptor(factory, 0)->id);
    plugin->init(plugin);
    auto* state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    auto* params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    auto load = [&](std::vector<uint8_t> data) { Buffer b{data}; clap_istream_t s{&b, readFn}; return state->load(plugin, &s); };
    auto value = [&](clap_id id) { double v = -1; params->get_value(plugin, id, &v); return v; };
    int failures = 0;
    auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); failures += !ok; };

    // Old 4-parameter state (pre-mic build): custom values must load; new params get defaults.
    check(load(blob({{1, 1.3}, {2, 0.8}, {3, 0.6}, {4, 1.5}})), "old 4-parameter state loads");
    check(value(1) == 1.3 && value(2) == 0.8 && value(3) == 0.6 && value(4) == 1.5, "old values restored");
    check(value(5) == 0.0 && value(6) == 1.0, "new parameters default (Spaced Pair, width 1)");
    check(value(7) == 1.0 && value(9) == 1.0 && value(13) == 1.0 && value(17) == 1.0, "voicing parameters default to 1");
    auto* remote = static_cast<const clap_plugin_remote_controls_t*>(plugin->get_extension(plugin, CLAP_EXT_REMOTE_CONTROLS));
    clap_remote_controls_page_t page{};
    bool pagesValid = remote != nullptr && remote->count(plugin) == 3;
    for (uint32_t p = 0; pagesValid && p < 3; ++p) {
        pagesValid = remote->get(plugin, p, &page);
        for (clap_id id : page.param_ids) {
            double v; pagesValid = pagesValid && (id == CLAP_INVALID_ID || params->get_value(plugin, id, &v));
        }
    }
    check(pagesValid, "remote-control pages reference real parameters");
    clap_param_info_t info{};
    params->get_info(plugin, 0, &info);
    check(info.id == 2 && (info.flags & CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID), "Hammer Hardness is per-note modulatable");
    auto* voices = static_cast<const clap_plugin_voice_info_t*>(plugin->get_extension(plugin, CLAP_EXT_VOICE_INFO));
    clap_voice_info_t voiceInfo{};
    check(voices != nullptr && voices->get(plugin, &voiceInfo) && voiceInfo.voice_capacity == 96, "voice info reports 96 voices");
    // Built-in presets are discoverable as marker files in the bundle and load by file.
    auto* discovery = static_cast<const clap_preset_discovery_factory_t*>(entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
    check(discovery != nullptr && discovery->count(discovery) == 1, "preset discovery has one provider");
    clap_preset_discovery_indexer_t indexer{CLAP_VERSION, "harness", "", "", "1", nullptr,
                                            declareFiletype, declareLocation, declareSoundpack, indexerExtension};
    auto* provider = discovery->create(discovery, &indexer, discovery->get_descriptor(discovery, 0)->id);
    check(provider != nullptr && provider->init(provider) && discovered.locations == 1 &&
          discovered.kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && std::filesystem::is_directory(discovered.path),
          "provider declares the bundle's preset folder");
    clap_preset_discovery_metadata_receiver_t receiver{nullptr, onError, beginPreset, addPluginId, setText, setFlags,
                                                       setText, setText, setTimestamps, setText, addExtraInfo};
    bool everyFileRead = true;
    for (const auto& file : std::filesystem::directory_iterator(discovered.path)) {
        everyFileRead = everyFileRead && provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE,
                                                                file.path().string().c_str(), &receiver);
    }
    const auto presetsFor = [&](const char* id) { return std::count(discovered.pluginIds.begin(), discovered.pluginIds.end(), id); };
    check(everyFileRead && discovered.names.size() == 34 && discovered.pluginIds.size() == 34 && !discovered.keyed &&
          presetsFor("com.pianocraft.upright") == 4 && presetsFor("com.pianocraft.harp") == 4 &&
          presetsFor("com.pianocraft.flute") == 2 && presetsFor("com.pianocraft.strings") == 7 &&
          presetsFor("com.pianocraft.horn") == 3 && presetsFor("com.pianocraft.trumpet") == 2 &&
          presetsFor("com.pianocraft.trombone") == 2 && presetsFor("com.pianocraft.tuba") == 2 &&
          presetsFor("com.pianocraft.oboe") == 2 && presetsFor("com.pianocraft.clarinet") == 2 &&
          presetsFor("com.pianocraft.celesta") == 2 && presetsFor("com.pianocraft.crotales") == 2,
          "34 preset files, one unkeyed preset each: 4 piano, 4 harp, 2 flute, 7 strings, 3 horn, 2 trumpet, 2 trombone, "
          "2 tuba, 2 oboe, 2 clarinet, 2 celesta, 2 crotales");
    provider->destroy(provider);
    auto* presetLoad = static_cast<const clap_plugin_preset_load_t*>(plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD));
    auto loadPreset = [&](const char* file) {
        return presetLoad->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, (discovered.path + file).c_str(), nullptr);
    };
    check(presetLoad != nullptr && loadPreset("/most-worn-mono.pianocraft") &&
          value(16) == 1.0 && value(6) == 0.0 && value(1) == 0.7 && value(2) == 1.0, "Most Worn (Mono) loads over defaults");
    check(loadPreset("/calibrated.pianocraft") && value(16) == 0.0 && value(6) == 1.0, "Calibrated Upright restores defaults");
    check(!loadPreset("/no-such-preset.pianocraft") && !loadPreset("/calibrated.pianocraft.bak"), "unknown preset rejected");
    // Current state round-trips.
    check(load(blob({{1, 0.5}, {2, 1.0}, {3, 0.35}, {4, 1.0}, {5, 2.0}, {6, 0.4}})), "6-parameter state loads");
    Buffer out; clap_ostream_t os{&out, writeFn}; state->save(plugin, &os);
    check(load(out.data), "saved state reloads");
    check(value(5) == 2.0 && value(6) == 0.4, "mic and width round-trip");
    char text[64]; params->value_to_text(plugin, 5, 1.0, text, sizeof(text));
    double parsed = -1; params->text_to_value(plugin, 5, text, &parsed);
    check(std::strcmp(text, "Coincident") == 0 && parsed == 1.0, "mic text round-trips");
    // Malformed input is rejected.
    check(!load(blob({{1, 0.5}, {1, 0.5}})), "duplicate parameter rejected");
    check(!load(blob({{1, 0.5}}, true)), "trailing byte rejected");
    std::vector<std::pair<uint32_t,double>> all;
    for (uint32_t id = 1; id <= params->count(plugin) + 1; ++id) all.push_back({id, 1.0});
    check(!load(blob(all)), "too many records rejected");
    check(!load(blob({{99, 1.0}})), "unknown parameter rejected");
    check(!load(blob({{5, 7.0}})), "out-of-range mic rejected");
    check(!load(blob({})), "zero records rejected");
    // A choked note ends ~0.4 s later and must be reported with its note id.
    plugin->activate(plugin, 48000.0, 512, 512);
    plugin->start_processing(plugin);
    std::vector<float> left(512), right(512); float* channels[2] = {left.data(), right.data()};
    clap_audio_buffer_t audio{channels, nullptr, 2, 0, 0};
    Events input, output;
    input.notes = {noteEvent(CLAP_EVENT_NOTE_ON, 0, 5), noteEvent(CLAP_EVENT_NOTE_CHOKE, 10, 5)};
    clap_input_events_t in{&input, eventsSize, eventsGet};
    clap_output_events_t outEvents{&output, eventsPush};
    for (int block = 0; block < 60; ++block) {
        clap_process_t process{};
        process.steady_time = block * 512LL; process.frames_count = 512;
        process.audio_outputs = &audio; process.audio_outputs_count = 1;
        process.in_events = &in; process.out_events = &outEvents;
        plugin->process(plugin, &process);
        input.notes.clear();
    }
    check(output.notes.size() == 1 && output.notes[0].note_id == 5 && output.notes[0].key == 60 &&
          output.notes[0].header.time < 512, "choked note reported as NOTE_END");
    // Editor: a slider gesture reaches the host as begin, value, end, and the value is applied.
    {
        pianocraft::GuiEdits edits;
        edits.push({3, 0.0, pianocraft::EditPhase::begin});
        edits.push({3, 0.5, pianocraft::EditPhase::value});
        edits.push({3, 0.0, pianocraft::EditPhase::end});
        Recorded recorded; clap_output_events_t out{&recorded, recordPush};
        std::vector<std::pair<uint32_t, double>> applied;
        edits.drain(&out, [](uint32_t index) { return clap_id(100 + index); }, [&](uint32_t index, double v) { applied.emplace_back(index, v); });
        check(recorded.types == std::vector<uint16_t>{CLAP_EVENT_PARAM_GESTURE_BEGIN, CLAP_EVENT_PARAM_VALUE, CLAP_EVENT_PARAM_GESTURE_END}
                  && recorded.ids == std::vector<clap_id>{103, 103, 103} && recorded.values == std::vector<double>{0.5}
                  && applied == std::vector<std::pair<uint32_t, double>>{{3, 0.5}},
              "an editor gesture is applied once and reported as begin, value, end");
        Recorded again; clap_output_events_t out2{&again, recordPush};
        edits.drain(&out2, [](uint32_t index) { return clap_id(index); }, [](uint32_t, double) {});
        check(again.types.empty(), "drained edits are not reported twice");
        int accepted = 0;
        for (int i = 0; i < 300; ++i) accepted += edits.push({0, 1.0, pianocraft::EditPhase::value});
        check(accepted == 255, "a full edit queue refuses edits instead of overwriting");
    }
    // Editor through the ABI: Cocoa only, embedded and floating; an embedded editor has a size.
    {
        auto* gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
        bool apis = gui != nullptr && gui->is_api_supported(plugin, CLAP_WINDOW_API_COCOA, false)
                    && gui->is_api_supported(plugin, CLAP_WINDOW_API_COCOA, true) && !gui->is_api_supported(plugin, CLAP_WINDOW_API_X11, false);
        check(apis, "gui: cocoa embedded and floating, nothing else");
        uint32_t w = 0, h = 0;
        bool sized = gui != nullptr && gui->create(plugin, CLAP_WINDOW_API_COCOA, false) && gui->get_size(plugin, &w, &h) && w > 0 && h > 0
                     && !gui->create(plugin, CLAP_WINDOW_API_COCOA, false);
        if (gui != nullptr) gui->destroy(plugin);
        check(sized, "gui: an embedded editor is created once, has a size and is destroyed");
    }
    plugin->stop_processing(plugin); plugin->deactivate(plugin);
    plugin->destroy(plugin);

    // The other voices: state round trip, a preset over defaults, choice text, and NOTE_END.
    check(factory->get_plugin_count(factory) == 12,
          "the bundle holds piano, harp, flute, strings, horn, trumpet, trombone, tuba, oboe, clarinet, celesta and crotales");
    struct Other { const char* id; const char* preset; clap_id changed; double expected; clap_id choice; const char* choiceText; int16_t key = 60; };
    for (const Other& other : {Other{"com.pianocraft.harp", "/harp-harmonics-mono.pianocraft", 4, 1.0, 4, "On"},
                               Other{"com.pianocraft.flute", "/flute-veiled.pianocraft", 2, 1.8, CLAP_INVALID_ID, nullptr},
                               Other{"com.pianocraft.strings", "/strings-harmonics-mono.pianocraft", 6, 1.0, 6, "On"},
                               Other{"com.pianocraft.horn", "/horn-stopped-mono.pianocraft", 2, 1.0, 2, "On"},
                               Other{"com.pianocraft.trumpet", "/trumpet-muted-mono.pianocraft", 2, 1.0, 2, "On"},
                               Other{"com.pianocraft.trombone", "/trombone-muted-mono.pianocraft", 2, 1.0, 2, "On"},
                               Other{"com.pianocraft.tuba", "/tuba-mono.pianocraft", 4, 0.0, 2, "On"},
                               Other{"com.pianocraft.oboe", "/oboe-dolce.pianocraft", 5, 0.75, CLAP_INVALID_ID, nullptr},
                               Other{"com.pianocraft.clarinet", "/clarinet-dolce.pianocraft", 5, 0.75, CLAP_INVALID_ID, nullptr},
                               Other{"com.pianocraft.celesta", "/celesta-mono.pianocraft", 5, 0.0, CLAP_INVALID_ID, nullptr},
                               Other{"com.pianocraft.crotales", "/crotales-mono.pianocraft", 5, 0.0, CLAP_INVALID_ID, nullptr, 96}}) {
        auto* instance = factory->create_plugin(factory, &host, other.id);
        instance->init(instance);
        auto* otherState = static_cast<const clap_plugin_state_t*>(instance->get_extension(instance, CLAP_EXT_STATE));
        auto* otherParams = static_cast<const clap_plugin_params_t*>(instance->get_extension(instance, CLAP_EXT_PARAMS));
        auto* otherPresets = static_cast<const clap_plugin_preset_load_t*>(instance->get_extension(instance, CLAP_EXT_PRESET_LOAD));
        auto otherValue = [&](clap_id id) { double v = -1; otherParams->get_value(instance, id, &v); return v; };
        const std::string name = other.id;
        check(otherPresets->from_location(instance, CLAP_PRESET_DISCOVERY_LOCATION_FILE, (discovered.path + other.preset).c_str(), nullptr) &&
              otherValue(other.changed) == other.expected, (name + ": preset loads").c_str());
        check(!otherPresets->from_location(instance, CLAP_PRESET_DISCOVERY_LOCATION_FILE, (discovered.path + "/calibrated.pianocraft").c_str(), nullptr),
              (name + ": another instrument's preset is rejected").c_str());
        Buffer saved; clap_ostream_t savedStream{&saved, writeFn};
        Buffer reloaded{};
        const bool savedOk = otherState->save(instance, &savedStream);
        reloaded.data = saved.data;
        clap_istream_t reloadStream{&reloaded, readFn};
        check(savedOk && otherState->load(instance, &reloadStream) && otherValue(other.changed) == other.expected,
              (name + ": state round-trips").c_str());
        if (other.choice != CLAP_INVALID_ID) {
            char choiceText[64]; double choiceValue = -1;
            otherParams->value_to_text(instance, other.choice, 1.0, choiceText, sizeof(choiceText));
            otherParams->text_to_value(instance, other.choice, choiceText, &choiceValue);
            check(std::strcmp(choiceText, other.choiceText) == 0 && choiceValue == 1.0, (name + ": choice text round-trips").c_str());
        }
        instance->activate(instance, 48000.0, 512, 512);
        instance->start_processing(instance);
        Events otherInput, otherOutput;
        otherInput.notes = {noteEvent(CLAP_EVENT_NOTE_ON, 0, 7, other.key), noteEvent(CLAP_EVENT_NOTE_CHOKE, 4000 % 512, 7, other.key)};
        clap_input_events_t otherIn{&otherInput, eventsSize, eventsGet};
        clap_output_events_t otherOut{&otherOutput, eventsPush};
        float peak = 0.0F;
        for (int block = 0; block < 5; ++block) {
            clap_process_t process{};
            process.steady_time = block * 512LL; process.frames_count = 512;
            process.audio_outputs = &audio; process.audio_outputs_count = 1;
            process.in_events = &otherIn; process.out_events = &otherOut;
            if (block == 0) {
                otherInput.notes.pop_back(); // sound first (a horn takes ~30 ms to speak), choke later
            }
            if (block == 3) {
                otherInput.notes = {noteEvent(CLAP_EVENT_NOTE_CHOKE, 0, 7, other.key)};
            }
            instance->process(instance, &process);
            for (float sample : left) peak = std::max(peak, std::abs(sample));
            otherInput.notes.clear();
        }
        check(peak > 1.0e-4F && otherOutput.notes.size() == 1 && otherOutput.notes[0].note_id == 7,
              (name + ": sounds, and a choked note is reported as NOTE_END").c_str());
        instance->stop_processing(instance); instance->deactivate(instance);
        instance->destroy(instance);
    }
    entry->deinit();
    std::printf("%s\n", failures ? "FAILED" : "all state checks passed");
    return failures != 0;
}
