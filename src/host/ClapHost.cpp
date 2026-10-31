#include "../engine/PianoEngine.hpp"

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
#include <type_traits>
#include <utility>

namespace {

using pianocraft::NoteAddress;
using pianocraft::ParameterId;
using pianocraft::PianoEngine;

struct ParameterSpec {
    clap_id id;
    const char* name;
    const char* module;
    double minimum;
    double maximum;
    double defaultValue;
    clap_param_info_flags flags;
};

constexpr clap_param_info_flags continuousFlags = CLAP_PARAM_IS_AUTOMATABLE;
constexpr clap_param_info_flags choiceFlags =
    CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM;
constexpr clap_param_info_flags perNoteFlags = continuousFlags | CLAP_PARAM_IS_MODULATABLE |
    CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID | CLAP_PARAM_IS_MODULATABLE_PER_KEY |
    CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL | CLAP_PARAM_IS_MODULATABLE_PER_PORT;
constexpr std::array<const char*, 3> micPositionNames{"Spaced Pair", "Coincident", "Player"};

constexpr clap_id pid(ParameterId id) noexcept
{
    return static_cast<clap_id>(id);
}

// Listed by group; hosts identify parameters by id, so the order is free.
constexpr std::array<ParameterSpec, 16> parameterSpecs{{
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
    {pid(ParameterId::micPosition), "Mic Position", "Mix", 0.0, 2.0, 0.0, choiceFlags},
    {pid(ParameterId::stereoWidth), "Stereo Width", "Mix", 0.0, 2.0, 1.0, continuousFlags},
    {pid(ParameterId::outputGain), "Output Gain", "Mix", 0.0, 2.0, 0.7, continuousFlags},
}};

struct RemotePage {
    const char* name;
    std::array<clap_id, CLAP_REMOTE_CONTROLS_COUNT> parameters;
};

constexpr std::array<RemotePage, 2> remotePages{{
    {"Main", {pid(ParameterId::hammerHardness), pid(ParameterId::dynamicRange),
              pid(ParameterId::velocityBrightness), pid(ParameterId::sustainLength),
              pid(ParameterId::sympatheticLevel), pid(ParameterId::bodyLevel),
              pid(ParameterId::stereoWidth), pid(ParameterId::outputGain)}},
    {"Voicing", {pid(ParameterId::unisonDetune), pid(ParameterId::aftersound),
                 pid(ParameterId::damperRelease), pid(ParameterId::hammerNoise),
                 pid(ParameterId::micPosition), pid(ParameterId::masterTune), pid(ParameterId::stretch),
                 pid(ParameterId::age)}},
}};

// Built-in voicings for Bitwig's browser (CLAP preset discovery): every parameter at its default
// except those listed. The Old House score bounces mono, so its wear stages use Stereo Width 0.
struct Preset {
    const char* name;
    const char* loadKey;
    const char* description;
    std::array<std::pair<ParameterId, double>, 2> changes;
};

constexpr std::array<Preset, 4> presets{{
    {"Calibrated Upright", "calibrated", "The recorded upright as measured, spaced pair, stereo",
     {{{ParameterId::stereoWidth, 1.0}, {ParameterId::age, 0.0}}}},
    {"Whole (Mono)", "whole-mono", "Unworn, mono-safe for a single emitter",
     {{{ParameterId::stereoWidth, 0.0}, {ParameterId::age, 0.0}}}},
    {"Lightly Worn (Mono)", "lightly-worn-mono", "Age 0.3, mono-safe for a single emitter",
     {{{ParameterId::stereoWidth, 0.0}, {ParameterId::age, 0.3}}}},
    {"Most Worn (Mono)", "most-worn-mono", "Age 1, mono-safe for a single emitter",
     {{{ParameterId::stereoWidth, 0.0}, {ParameterId::age, 1.0}}}},
}};

constexpr std::size_t invalidParameterIndex = parameterSpecs.size();
constexpr std::size_t stateSize = 12 + parameterSpecs.size() * 12;
constexpr std::uint32_t stateVersion = 1;
static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == 8);
static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

std::size_t parameterIndex(clap_id id) noexcept
{
    for (std::size_t i = 0; i < parameterSpecs.size(); ++i) {
        if (parameterSpecs[i].id == id) {
            return i;
        }
    }
    return invalidParameterIndex;
}

double clampedParameterValue(std::size_t index, double value) noexcept
{
    return std::clamp(value, parameterSpecs[index].minimum, parameterSpecs[index].maximum);
}

void copyText(char* destination, std::size_t capacity, const char* source) noexcept
{
    if (capacity == 0) {
        return;
    }
    std::snprintf(destination, capacity, "%s", source);
}

void writeU32(std::array<std::uint8_t, stateSize>& bytes, std::size_t& offset,
              std::uint32_t value) noexcept
{
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes[offset++] = static_cast<std::uint8_t>(value >> shift);
    }
}

std::uint32_t readU32(const std::array<std::uint8_t, stateSize>& bytes,
                      std::size_t& offset) noexcept
{
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    }
    return value;
}

void writeDouble(std::array<std::uint8_t, stateSize>& bytes, std::size_t& offset,
                 double value) noexcept
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes[offset++] = static_cast<std::uint8_t>(bits >> shift);
    }
}

double readDouble(const std::array<std::uint8_t, stateSize>& bytes,
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

        for (std::size_t i = 0; i < parameterSpecs.size(); ++i) {
            parameterValues_[i].store(parameterSpecs[i].defaultValue, std::memory_order_relaxed);
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
        delete &self(plugin);
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
        for (std::size_t i = 0; i < parameterSpecs.size(); ++i) {
            engine_.setParameter(static_cast<ParameterId>(parameterSpecs[i].id),
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

        for (std::size_t i = 0; i < parameterSpecs.size(); ++i) {
            engine_.setParameter(static_cast<ParameterId>(parameterSpecs[i].id),
                                 parameterValues_[i].load(std::memory_order_relaxed));
        }
        appliedParameterRevision_ = revision;
    }

    void setParameter(std::size_t index, double value, bool updateEngine) noexcept
    {
        if (!std::isfinite(value)) {
            return;
        }
        const double clamped = clampedParameterValue(index, value);
        parameterValues_[index].store(clamped, std::memory_order_relaxed);
        parameterRevision_.fetch_add(1, std::memory_order_release);
        if (updateEngine) {
            engine_.setParameter(static_cast<ParameterId>(parameterSpecs[index].id), clamped);
        }
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

        if (status == 0xB0U && event.data[1] == 64U) {
            engine_.setSustain(channel, event.data[2] / 127.0);
            return;
        }
        if (status == 0xB0U && event.data[1] == 67U) {
            engine_.setSoftPedal(channel, event.data[2] / 127.0);
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
            const auto index = parameterIndex(event.param_id);
            if (index != invalidParameterIndex) {
                setParameter(index, event.value, true);
            }
            return;
        }

        if (header->type == CLAP_EVENT_PARAM_MOD) {
            if (header->size < sizeof(clap_event_param_mod_t)) {
                return;
            }
            const auto& event = *reinterpret_cast<const clap_event_param_mod_t*>(header);
            if (parameterIndex(event.param_id) != invalidParameterIndex) {
                engine_.modulate(NoteAddress{event.note_id, event.port_index, event.channel, event.key},
                                 static_cast<ParameterId>(event.param_id), event.amount);
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
        return static_cast<std::uint32_t>(parameterSpecs.size());
    }

    static bool paramsGetInfo(const clap_plugin_t*, std::uint32_t index,
                              clap_param_info_t* info) noexcept
    {
        if (index >= parameterSpecs.size() || info == nullptr) {
            return false;
        }
        const auto& spec = parameterSpecs[index];
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
        const auto index = parameterIndex(id);
        if (index == invalidParameterIndex || value == nullptr) {
            return false;
        }
        *value = self(plugin).parameterValues_[index].load(std::memory_order_acquire);
        return true;
    }

    static bool paramsValueToText(const clap_plugin_t*, clap_id id, double value,
                                  char* output, std::uint32_t capacity) noexcept
    {
        const auto index = parameterIndex(id);
        if (index == invalidParameterIndex || output == nullptr || capacity == 0 ||
            !std::isfinite(value)) {
            return false;
        }
        const double clamped = clampedParameterValue(index, value);
        const auto result = parameterSpecs[index].id == static_cast<clap_id>(ParameterId::micPosition)
            ? std::snprintf(output, capacity, "%s", micPositionNames[static_cast<std::size_t>(std::lround(clamped))])
            : std::snprintf(output, capacity, "%.6g", clamped);
        return result >= 0 && static_cast<std::uint32_t>(result) < capacity;
    }

    static bool paramsTextToValue(const clap_plugin_t*, clap_id id, const char* text,
                                  double* value) noexcept
    {
        const auto index = parameterIndex(id);
        if (index == invalidParameterIndex || text == nullptr || value == nullptr) {
            return false;
        }
        if (id == static_cast<clap_id>(ParameterId::micPosition)) {
            for (std::size_t i = 0; i < micPositionNames.size(); ++i) {
                if (std::strcmp(text, micPositionNames[i]) == 0) {
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
        *value = clampedParameterValue(index, parsed);
        return true;
    }

    static void paramsFlush(const clap_plugin_t* plugin, const clap_input_events_t* input,
                            const clap_output_events_t*) noexcept
    {
        auto& adapter = self(plugin);
        adapter.synchronizeParameters();
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
        std::array<std::uint8_t, stateSize> bytes{};
        std::size_t offset = 0;
        bytes[offset++] = static_cast<std::uint8_t>('P');
        bytes[offset++] = static_cast<std::uint8_t>('C');
        bytes[offset++] = static_cast<std::uint8_t>('S');
        bytes[offset++] = static_cast<std::uint8_t>('T');
        writeU32(bytes, offset, stateVersion);
        writeU32(bytes, offset, static_cast<std::uint32_t>(parameterSpecs.size()));
        for (std::size_t i = 0; i < parameterSpecs.size(); ++i) {
            writeU32(bytes, offset, parameterSpecs[i].id);
            writeDouble(bytes, offset, adapter.parameterValues_[i].load(std::memory_order_acquire));
        }
        return writeExactly(stream, bytes.data(), bytes.size());
    }

    static bool stateLoad(const clap_plugin_t* plugin, const clap_istream_t* stream) noexcept
    {
        auto& adapter = self(plugin);
        // States from older versions carry fewer parameters; the missing ones keep defaults.
        std::array<std::uint8_t, stateSize> bytes{};
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
        if (recordCount == 0 || recordCount > parameterSpecs.size() ||
            !readExactly(stream, bytes.data() + headerSize, recordCount * recordSize)) {
            return false;
        }

        std::array<double, parameterSpecs.size()> loadedValues{};
        for (std::size_t i = 0; i < parameterSpecs.size(); ++i) {
            loadedValues[i] = parameterSpecs[i].defaultValue;
        }
        std::array<bool, parameterSpecs.size()> seen{};
        for (std::size_t record = 0; record < recordCount; ++record) {
            const auto index = parameterIndex(readU32(bytes, offset));
            const double value = readDouble(bytes, offset);
            if (index == invalidParameterIndex || seen[index] || !std::isfinite(value) ||
                value < parameterSpecs[index].minimum || value > parameterSpecs[index].maximum) {
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
    void adopt(const std::array<double, parameterSpecs.size()>& values) noexcept
    {
        for (std::size_t i = 0; i < parameterSpecs.size(); ++i) {
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
        const auto* preset = locationKind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && loadKey != nullptr
            ? std::find_if(presets.begin(), presets.end(),
                           [loadKey](const Preset& p) { return std::strcmp(p.loadKey, loadKey) == 0; })
            : presets.end();
        const clap_host_preset_load_t* hostPresets = nullptr;
        if (adapter.host_ != nullptr && adapter.host_->get_extension != nullptr) {
            hostPresets = static_cast<const clap_host_preset_load_t*>(
                adapter.host_->get_extension(adapter.host_, CLAP_EXT_PRESET_LOAD));
        }
        if (preset == presets.end()) {
            if (hostPresets != nullptr && hostPresets->on_error != nullptr) {
                hostPresets->on_error(adapter.host_, locationKind, location, loadKey, 0, "Unknown PianoCraft preset");
            }
            return false;
        }
        std::array<double, parameterSpecs.size()> values{};
        for (std::size_t i = 0; i < parameterSpecs.size(); ++i) {
            values[i] = parameterSpecs[i].defaultValue;
        }
        for (const auto& [id, value] : preset->changes) {
            values[parameterIndex(pid(id))] = value;
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
        std::array<PianoEngine::EndedNote, pianocraft::voiceCapacity> ended{};
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
        info->voice_count = static_cast<std::uint32_t>(pianocraft::voiceCapacity);
        info->voice_capacity = info->voice_count;
        info->flags = CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES;
        return true;
    }

    static std::uint32_t remoteControlsCount(const clap_plugin_t*) noexcept
    {
        return static_cast<std::uint32_t>(remotePages.size());
    }

    static bool remoteControlsGet(const clap_plugin_t*, std::uint32_t index,
                                  clap_remote_controls_page_t* page) noexcept
    {
        if (index >= remotePages.size() || page == nullptr) {
            return false;
        }
        std::memset(page, 0, sizeof(*page));
        copyText(page->section_name, sizeof(page->section_name), "Piano");
        page->page_id = index;
        copyText(page->page_name, sizeof(page->page_name), remotePages[index].name);
        std::copy(remotePages[index].parameters.begin(), remotePages[index].parameters.end(), page->param_ids);
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
        (void)plugin;
        return nullptr;
    }

    static void onMainThread(const clap_plugin_t*) noexcept {}

    const clap_host_t* host_;
    PianoEngine engine_;
    clap_plugin_t plugin_{};
    std::array<std::atomic<double>, parameterSpecs.size()> parameterValues_{};
    std::atomic<std::uint32_t> parameterRevision_{1};
    std::uint32_t appliedParameterRevision_ = 0;
    bool active_ = false;
    bool processing_ = false;

    static const clap_plugin_descriptor_t descriptor_;
    static const clap_plugin_audio_ports_t audioPorts_;
    static const clap_plugin_note_ports_t notePorts_;
    static const clap_plugin_params_t params_;
    static const clap_plugin_state_t state_;
    static const clap_plugin_remote_controls_t remoteControls_;
    static const clap_plugin_voice_info_t voiceInfo_;
    static const clap_plugin_preset_load_t presetLoad_;
};

constexpr const char* const pluginFeatures[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_SYNTHESIZER,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

const clap_plugin_descriptor_t Adapter::descriptor_ = {
    CLAP_VERSION_INIT,
    "com.pianocraft.upright",
    "PianoCraft Upright",
    "PianoCraft",
    "",
    "",
    "",
    "0.1.0",
    "Original upright piano instrument",
    pluginFeatures,
};

const clap_plugin_audio_ports_t Adapter::audioPorts_ = {
    &Adapter::audioPortsCount,
    &Adapter::audioPortsGet,
};

const clap_plugin_note_ports_t Adapter::notePorts_ = {
    &Adapter::notePortsCount,
    &Adapter::notePortsGet,
};

const clap_plugin_params_t Adapter::params_ = {
    &Adapter::paramsCount,
    &Adapter::paramsGetInfo,
    &Adapter::paramsGetValue,
    &Adapter::paramsValueToText,
    &Adapter::paramsTextToValue,
    &Adapter::paramsFlush,
};

const clap_plugin_state_t Adapter::state_ = {
    &Adapter::stateSave,
    &Adapter::stateLoad,
};

const clap_plugin_remote_controls_t Adapter::remoteControls_ = {
    &Adapter::remoteControlsCount,
    &Adapter::remoteControlsGet,
};

const clap_plugin_voice_info_t Adapter::voiceInfo_ = {
    &Adapter::voiceInfoGet,
};

const clap_plugin_preset_load_t Adapter::presetLoad_ = {
    &Adapter::presetLoad,
};

std::uint32_t pluginCount(const clap_plugin_factory_t*) noexcept
{
    return 1;
}

const clap_plugin_descriptor_t* pluginDescriptor(const clap_plugin_factory_t*,
                                                 std::uint32_t index) noexcept
{
    return index == 0 ? Adapter::descriptor() : nullptr;
}

const clap_plugin_t* createPlugin(const clap_plugin_factory_t*, const clap_host_t* host,
                                  const char* pluginId) noexcept
{
    if (host == nullptr || pluginId == nullptr ||
        !clap_version_is_compatible(host->clap_version) ||
        std::strcmp(pluginId, "com.pianocraft.upright") != 0) {
        return nullptr;
    }

    try {
        return (new Adapter(host))->plugin();
    } catch (...) {
        return nullptr;
    }
}

const clap_plugin_factory_t pluginFactory = {
    &pluginCount,
    &pluginDescriptor,
    &createPlugin,
};

// Preset discovery: one provider whose presets live inside the plugin, loaded by key.
const clap_preset_discovery_provider_descriptor_t presetProviderDescriptor = {
    CLAP_VERSION_INIT,
    "com.pianocraft.upright.presets",
    "PianoCraft presets",
    "PianoCraft",
};

bool presetProviderInit(const clap_preset_discovery_provider_t* provider) noexcept
{
    const auto* indexer = static_cast<const clap_preset_discovery_indexer_t*>(provider->provider_data);
    const clap_preset_discovery_location_t location{
        CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "PianoCraft", CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr};
    return indexer->declare_location(indexer, &location);
}

void presetProviderDestroy(const clap_preset_discovery_provider_t* provider) noexcept
{
    delete provider;
}

bool presetProviderGetMetadata(const clap_preset_discovery_provider_t*, std::uint32_t locationKind, const char*,
                               const clap_preset_discovery_metadata_receiver_t* receiver) noexcept
{
    if (locationKind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || receiver == nullptr) {
        return false;
    }
    const clap_universal_plugin_id_t pluginId{"clap", "com.pianocraft.upright"};
    for (const auto& preset : presets) {
        if (!receiver->begin_preset(receiver, preset.name, preset.loadKey)) {
            break;
        }
        receiver->add_plugin_id(receiver, &pluginId);
        receiver->set_flags(receiver, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
        receiver->set_description(receiver, preset.description);
    }
    return true;
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

bool entryInit(const char*) noexcept
{
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
