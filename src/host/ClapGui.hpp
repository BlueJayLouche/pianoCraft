#pragma once

// The editor: one generic AppKit panel for every instrument (ClapGui.mm), a column of the
// parameters grouped by module. Not a template, so ClapHost.cpp's per-instrument adapters describe
// their parameters and pass callbacks. Every function here is main-thread only.

#include "GuiEdits.hpp"

#include <cstddef>
#include <cstdint>

namespace pianocraft::gui {

struct Param {
    const char* name;
    const char* module;
    double minimum;
    double maximum;
    bool stepped;
    const char* const* choices; // a stepped parameter's value names from 0, or null
};

struct Client {
    void* context;
    double (*value)(void* context, std::size_t index);
    void (*edit)(void* context, std::size_t index, double value, EditPhase phase);
    bool (*text)(void* context, std::size_t index, double value, char* out, std::uint32_t capacity);
    void (*closed)(void* context); // the user closed a floating editor's window
};

struct Editor;

// Null on failure. `floating` makes the editor its own window, titled `title`.
Editor* create(const Param* params, std::size_t count, Client client, bool floating, const char* title);
void destroy(Editor* editor);
void size(const Editor* editor, std::uint32_t* width, std::uint32_t* height); // points
bool setParent(Editor* editor, void* nsView);
void setTitle(Editor* editor, const char* title);
bool show(Editor* editor);
bool hide(Editor* editor);

} // namespace pianocraft::gui
