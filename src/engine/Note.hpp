#pragma once

#include <cstdint>

namespace pianocraft {

struct NoteAddress {
    std::int32_t noteId = -1;
    std::int16_t port = 0;
    std::int16_t channel = 0;
    std::int16_t key = 60;
};

// A note whose voice stopped sounding; frame counts from the start of the latest render() call.
struct EndedNote {
    NoteAddress address;
    std::uint32_t frame = 0;
};

// CLAP per-note targeting: a negative noteId, port, channel or key matches any.
inline bool matches(const NoteAddress& target, const NoteAddress& note) {
    return (target.noteId < 0 || target.noteId == note.noteId) && (target.port < 0 || target.port == note.port) &&
           (target.channel < 0 || target.channel == note.channel) && (target.key < 0 || target.key == note.key);
}

} // namespace pianocraft
