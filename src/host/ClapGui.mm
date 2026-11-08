// The editor (ClapGui.hpp): a native AppKit column of the instrument's parameters, grouped by
// module. Sliders for continuous and stepped values, a pop-up for named choices. Built with ARC.

#import <Cocoa/Cocoa.h>

#include "ClapGui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <new>
#include <string>
#include <vector>

using pianocraft::EditPhase;
using pianocraft::gui::Client;
using pianocraft::gui::Param;

// Objective-C classes share one namespace in the host process: the version in the names keeps a
// second PianoCraft build loaded beside this one from clashing.
#define PCController PianoCraftGuiController_0_1_0
#define PCFlippedView PianoCraftGuiFlippedView_0_1_0

@interface PCFlippedView : NSView
@end

@implementation PCFlippedView
- (BOOL)isFlipped
{
    return YES;
}
@end

@interface PCController : NSObject <NSWindowDelegate>
@property(nonatomic, assign) pianocraft::gui::Editor* editor;
@end

struct pianocraft::gui::Editor {
    Client client{};
    std::vector<Param> params;
    PCController* controller = nil;
    NSView* view = nil;
    NSWindow* window = nil; // a floating editor's own window
    NSTimer* timer = nil;
    std::vector<NSControl*> controls; // per parameter: an NSSlider or an NSPopUpButton
    std::vector<NSTextField*> values; // per parameter: the value text (nil beside a pop-up)
    long editing = -1;                // the slider in a gesture
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

namespace {

using Editor = pianocraft::gui::Editor;

constexpr CGFloat panelWidth = 460;
constexpr CGFloat margin = 16;
constexpr CGFloat rowHeight = 26;
constexpr CGFloat headingHeight = 30;

bool isChoice(const Param& p)
{
    return p.choices != nullptr;
}

NSTextField* label(NSString* text, NSRect frame, NSFont* font, NSTextAlignment alignment)
{
    NSTextField* field = [NSTextField labelWithString:text];
    field.frame = frame;
    field.font = font;
    field.alignment = alignment;
    field.lineBreakMode = NSLineBreakByTruncatingTail;
    return field;
}

void showValue(Editor* e, std::size_t i, double value)
{
    if (isChoice(e->params[i])) {
        [((NSPopUpButton*)e->controls[i]) selectItemAtIndex:std::lround(value)];
        return;
    }
    ((NSSlider*)e->controls[i]).doubleValue = value;
    char text[64] = {};
    if (!e->client.text(e->client.context, i, value, text, sizeof(text))) {
        std::snprintf(text, sizeof(text), "%.3g", value);
    }
    e->values[i].stringValue = [NSString stringWithUTF8String:text];
}

void endGesture(Editor* e)
{
    if (e->editing >= 0) {
        e->client.edit(e->client.context, static_cast<std::size_t>(e->editing), 0.0, EditPhase::end);
        e->editing = -1;
    }
}

} // namespace

@implementation PCController

- (void)slid:(NSSlider*)slider
{
    Editor* e = self.editor;
    if (e == nullptr) {
        return;
    }
    const auto i = static_cast<long>(slider.tag);
    const NSEventType type = NSApp.currentEvent.type;
    const bool dragging = type == NSEventTypeLeftMouseDown || type == NSEventTypeLeftMouseDragged;
    if (e->editing != i) {
        endGesture(e);
        e->client.edit(e->client.context, static_cast<std::size_t>(i), 0.0, EditPhase::begin);
        e->editing = i;
    }
    e->client.edit(e->client.context, static_cast<std::size_t>(i), slider.doubleValue, EditPhase::value);
    showValue(e, static_cast<std::size_t>(i), slider.doubleValue);
    if (!dragging) {
        endGesture(e); // a click, a key or the end of a drag
    }
}

- (void)picked:(NSPopUpButton*)popUp
{
    Editor* e = self.editor;
    if (e == nullptr) {
        return;
    }
    const auto i = static_cast<std::size_t>(popUp.tag);
    endGesture(e);
    e->client.edit(e->client.context, i, 0.0, EditPhase::begin);
    e->client.edit(e->client.context, i, static_cast<double>(popUp.indexOfSelectedItem), EditPhase::value);
    e->client.edit(e->client.context, i, 0.0, EditPhase::end);
}

// Follows the host (automation, presets, its own controls) at 30 Hz, except the slider in a
// gesture; ends that gesture once the mouse is up, should the slider's last action be missed.
- (void)tick:(NSTimer*)timer
{
    Editor* e = self.editor;
    if (e == nullptr) {
        return;
    }
    if (e->editing >= 0 && NSEvent.pressedMouseButtons == 0) {
        endGesture(e);
    }
    for (std::size_t i = 0; i < e->params.size(); ++i) {
        if (static_cast<long>(i) == e->editing) {
            continue;
        }
        const double value = e->client.value(e->client.context, i);
        const double shown = isChoice(e->params[i]) ? static_cast<double>([((NSPopUpButton*)e->controls[i]) indexOfSelectedItem])
                                                    : e->controls[i].doubleValue;
        if (value != shown) {
            showValue(e, i, value);
        }
    }
}

// The host is told on the next turn of the run loop, so a host that destroys the editor from
// `closed` does not close this window again while AppKit is closing it.
- (void)windowWillClose:(NSNotification*)notification
{
    if (self.editor == nullptr) {
        return;
    }
    endGesture(self.editor);
    dispatch_async(dispatch_get_main_queue(), ^{
      Editor* e = self.editor;
      if (e != nullptr) {
          e->client.closed(e->client.context);
      }
    });
}

@end

namespace pianocraft::gui {

Editor* create(const Param* params, std::size_t count, Client client, bool floating, const char* title)
{
    if (params == nullptr || client.value == nullptr || client.edit == nullptr || client.text == nullptr || client.closed == nullptr) {
        return nullptr;
    }
    auto* e = new (std::nothrow) Editor{};
    if (e == nullptr) {
        return nullptr;
    }
    e->client = client;
    e->params.assign(params, params + count);
    e->controls.resize(count, nil);
    e->values.resize(count, nil);
    e->controller = [PCController new];
    e->controller.editor = e;

    // Modules in first-seen order, each with its parameters in table order.
    std::vector<std::string> modules;
    for (const auto& p : e->params) {
        if (std::find(modules.begin(), modules.end(), p.module) == modules.end()) {
            modules.emplace_back(p.module);
        }
    }
    // ponytail: no scroll view; the largest instrument (17 parameters) is about 640 points tall.
    // Add one if a table outgrows a laptop screen.
    const CGFloat height = margin * 2 + static_cast<CGFloat>(modules.size()) * headingHeight + static_cast<CGFloat>(count) * rowHeight;
    NSView* view = [[PCFlippedView alloc] initWithFrame:NSMakeRect(0, 0, panelWidth, height)];
    NSFont* small = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    CGFloat y = margin;
    for (const auto& module : modules) {
        [view addSubview:label([NSString stringWithUTF8String:module.c_str()], NSMakeRect(margin, y + 8, panelWidth - 2 * margin, 18),
                               [NSFont boldSystemFontOfSize:NSFont.systemFontSize], NSTextAlignmentLeft)];
        y += headingHeight;
        for (std::size_t i = 0; i < count; ++i) {
            const Param& p = e->params[i];
            if (module != p.module) {
                continue;
            }
            [view addSubview:label([NSString stringWithUTF8String:p.name], NSMakeRect(margin, y + 4, 140, 18), small, NSTextAlignmentLeft)];
            const NSRect control = NSMakeRect(margin + 146, y, 210, 22);
            if (isChoice(p)) {
                NSPopUpButton* popUp = [[NSPopUpButton alloc] initWithFrame:control pullsDown:NO];
                popUp.controlSize = NSControlSizeSmall;
                popUp.font = small;
                for (long c = 0; c <= std::lround(p.maximum); ++c) {
                    [popUp addItemWithTitle:[NSString stringWithUTF8String:p.choices[c]]];
                }
                popUp.target = e->controller;
                popUp.action = @selector(picked:);
                e->controls[i] = popUp;
            } else {
                NSSlider* slider = [NSSlider sliderWithValue:p.minimum minValue:p.minimum maxValue:p.maximum target:e->controller action:@selector(slid:)];
                slider.frame = control;
                slider.controlSize = NSControlSizeSmall;
                slider.continuous = YES;
                if (p.stepped) {
                    slider.numberOfTickMarks = std::lround(p.maximum - p.minimum) + 1;
                    slider.allowsTickMarkValuesOnly = YES;
                }
                e->controls[i] = slider;
                e->values[i] = label(@"", NSMakeRect(margin + 362, y + 4, panelWidth - margin * 2 - 362, 18), small, NSTextAlignmentRight);
                [view addSubview:e->values[i]];
            }
            e->controls[i].tag = static_cast<NSInteger>(i);
            [view addSubview:e->controls[i]];
            showValue(e, i, client.value(client.context, i));
            y += rowHeight;
        }
    }
    e->view = view;
    e->width = static_cast<std::uint32_t>(panelWidth);
    e->height = static_cast<std::uint32_t>(std::ceil(height));

    if (floating) {
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, panelWidth, height)
                                                       styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                         backing:NSBackingStoreBuffered
                                                           defer:YES];
        window.releasedWhenClosed = NO;
        window.contentView = view;
        window.delegate = e->controller;
        window.title = [NSString stringWithUTF8String:title != nullptr ? title : "PianoCraft"];
        [window center];
        e->window = window;
    }
    e->timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30.0 target:e->controller selector:@selector(tick:) userInfo:nil repeats:YES];
    return e;
}

void destroy(Editor* e)
{
    if (e == nullptr) {
        return;
    }
    endGesture(e);
    [e->timer invalidate]; // the timer retains the controller
    if (e->window != nil) {
        e->window.delegate = nil; // no `closed` for a close the host asked for
        [e->window orderOut:nil];
        [e->window close];
    }
    [e->view removeFromSuperview];
    e->controller.editor = nullptr;
    delete e;
}

void size(const Editor* e, std::uint32_t* width, std::uint32_t* height)
{
    *width = e->width;
    *height = e->height;
}

bool setParent(Editor* e, void* nsView)
{
    NSView* parent = (__bridge NSView*)nsView;
    if (parent == nil || e->window != nil) {
        return false;
    }
    e->view.frameOrigin = NSZeroPoint;
    [parent addSubview:e->view];
    return true;
}

void setTitle(Editor* e, const char* title)
{
    if (e->window != nil && title != nullptr) {
        e->window.title = [NSString stringWithUTF8String:title];
    }
}

bool show(Editor* e)
{
    if (e->window != nil) {
        [e->window makeKeyAndOrderFront:nil];
    } else {
        e->view.hidden = NO;
    }
    return true;
}

bool hide(Editor* e)
{
    endGesture(e);
    if (e->window != nil) {
        [e->window orderOut:nil];
    } else {
        e->view.hidden = YES;
    }
    return true;
}

} // namespace pianocraft::gui
