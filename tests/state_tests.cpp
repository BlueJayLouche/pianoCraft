// Loads the built CLAP binary and checks state compatibility: states saved before the mic
// parameters existed must still load, and malformed state must be rejected.
#include <clap/clap.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <vector>

struct Buffer { std::vector<uint8_t> data; size_t pos = 0; };
static int64_t readFn(const clap_istream_t* s, void* out, uint64_t size) {
    auto* b = static_cast<Buffer*>(s->ctx); size_t n = std::min<size_t>(size, b->data.size() - b->pos);
    std::memcpy(out, b->data.data() + b->pos, n); b->pos += n; return (int64_t)n;
}
static int64_t writeFn(const clap_ostream_t* s, const void* in, uint64_t size) {
    auto* b = static_cast<Buffer*>(s->ctx); auto* p = static_cast<const uint8_t*>(in); b->data.insert(b->data.end(), p, p + size); return (int64_t)size;
}
static const void* hostExt(const clap_host_t*, const char*) { return nullptr; }
static void noop(const clap_host_t*) {}
static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xff); }
static void putD(std::vector<uint8_t>& v, double d) { uint64_t b; std::memcpy(&b, &d, 8); for (int i = 0; i < 8; ++i) v.push_back((b >> (8 * i)) & 0xff); }
static std::vector<uint8_t> blob(std::vector<std::pair<uint32_t,double>> recs, bool trailing = false) {
    std::vector<uint8_t> v{'P','C','S','T'}; put32(v, 1); put32(v, (uint32_t)recs.size());
    for (auto& [id, val] : recs) { put32(v, id); putD(v, val); }
    if (trailing) v.push_back(0); return v;
}
int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: pianocraft-state-tests <plugin binary>\n"); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW); if (!lib) { std::printf("dlopen failed: %s\n", dlerror()); return 1; }
    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry"));
    entry->init(argv[1]);
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
    check(value(7) == 1.0 && value(9) == 1.0 && value(13) == 1.0, "voicing parameters default to 1");
    auto* remote = static_cast<const clap_plugin_remote_controls_t*>(plugin->get_extension(plugin, CLAP_EXT_REMOTE_CONTROLS));
    clap_remote_controls_page_t page{};
    bool pagesValid = remote != nullptr && remote->count(plugin) == 2;
    for (uint32_t p = 0; pagesValid && p < 2; ++p) {
        pagesValid = remote->get(plugin, p, &page);
        for (clap_id id : page.param_ids) {
            double v; pagesValid = pagesValid && (id == CLAP_INVALID_ID || params->get_value(plugin, id, &v));
        }
    }
    check(pagesValid, "remote-control pages reference real parameters");
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
    for (uint32_t id = 1; id <= 14; ++id) all.push_back({id, 1.0});
    check(!load(blob(all)), "too many records rejected");
    check(!load(blob({{99, 1.0}})), "unknown parameter rejected");
    check(!load(blob({{5, 7.0}})), "out-of-range mic rejected");
    check(!load(blob({})), "zero records rejected");
    plugin->destroy(plugin); entry->deinit();
    std::printf("%s\n", failures ? "FAILED" : "all state checks passed");
    return failures != 0;
}
