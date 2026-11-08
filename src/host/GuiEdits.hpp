#pragma once

// Editor edits on their way from the editor (main thread) to process() or params.flush(), which
// apply them and report them to the host as gesture and value events, so hosts record automation
// and undo the editor's moves as they do their own controls. Single producer, single consumer,
// fixed size: no locks or allocation on the audio thread.

#include <clap/clap.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace pianocraft {

enum class EditPhase : std::uint8_t { begin, value, end };

struct GuiEdit {
    std::uint32_t index; // into the instrument's parameter table
    double value;        // used by EditPhase::value
    EditPhase phase;
};

class GuiEdits {
public:
    // Main thread. False when full: the edit is dropped.
    // ponytail: 256 slots, drained every block or flush; a host that neither processes nor flushes
    // after request_flush would drop edits past that. Grow it if a host ever does.
    bool push(GuiEdit edit) noexcept
    {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto next = (tail + 1) % capacity;
        if (next == head_.load(std::memory_order_acquire)) {
            return false;
        }
        ring_[tail] = edit;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // process() or params.flush() (never concurrently): calls apply(index, value) for each value
    // edit and reports every edit to `out` as CLAP gesture and value events for `idOf(index)`.
    template <class IdOf, class Apply>
    void drain(const clap_output_events_t* out, IdOf&& idOf, Apply&& apply) noexcept
    {
        auto head = head_.load(std::memory_order_relaxed);
        const auto tail = tail_.load(std::memory_order_acquire);
        for (; head != tail; head = (head + 1) % capacity) {
            const GuiEdit& edit = ring_[head];
            if (edit.phase == EditPhase::value) {
                apply(edit.index, edit.value);
            }
            report(out, idOf(edit.index), edit);
        }
        head_.store(head, std::memory_order_release);
    }

private:
    static void report(const clap_output_events_t* out, clap_id id, const GuiEdit& edit) noexcept
    {
        if (out == nullptr || out->try_push == nullptr) {
            return;
        }
        if (edit.phase == EditPhase::value) {
            clap_event_param_value_t event{};
            event.header = {sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            event.param_id = id;
            event.note_id = -1;
            event.port_index = -1;
            event.channel = -1;
            event.key = -1;
            event.value = edit.value;
            out->try_push(out, &event.header);
            return;
        }
        clap_event_param_gesture_t event{};
        const auto type = edit.phase == EditPhase::begin ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END;
        event.header = {sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID, static_cast<std::uint16_t>(type), 0};
        event.param_id = id;
        out->try_push(out, &event.header);
    }

    static constexpr std::uint32_t capacity = 256;
    std::array<GuiEdit, capacity> ring_{};
    std::atomic<std::uint32_t> head_{0};
    std::atomic<std::uint32_t> tail_{0};
};

} // namespace pianocraft
