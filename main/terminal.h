// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <esp_err.h>
#include <ghostty/vt.h>

#include "state.h"

// PSRAM; libghostty-vt has no allocator of its own without an OS
extern const GhosttyAllocator terminal_allocator;

// One SSH session's screen: written by its task, drawn by the screen's
typedef struct terminal terminal;

// NULL when out of memory
terminal* terminal_new(const terminal_size* size);
void terminal_free(terminal* t);
void terminal_resize(terminal* t, const terminal_size* size);
// The terminal keys are encoded for
void terminal_show(terminal* t);
// From the SSH channel; one task only, with terminal_reply
void terminal_write(terminal* t, const void* data, size_t size);
// The terminal's answers to queries, for the SSH channel
size_t terminal_reply(terminal* t, void* buffer, size_t size);
// Key bytes for the SSH channel, as the shown terminal's modes want them
size_t terminal_encode_key(GhosttyKeyEvent event, char* out, size_t size);
// From the render task, at pixel (x, y) of t: a left click, and `lines` turns of the wheel (up negative), as mouse reports
// for terminal_read_mouse when its program asked for them. Without them the wheel is Up/Down keys on the alternate screen
// if `arrows`, else it scrolls t's scrollback until terminal_scroll_to_bottom.
void terminal_click(terminal* t, int32_t x, int32_t y);
void terminal_wheel(terminal* t, int32_t lines, int32_t x, int32_t y, bool arrows);
void terminal_scroll_to_bottom(terminal* t);
// The mouse reports and the wheel's keys, for the SSH channel
size_t terminal_read_mouse(terminal* t, void* buffer, size_t size);
// From the render task: the terminal's state, safe to read until the next call; rows that only moved are not dirty,
// so its row moves must be replayed first. `images` gets the kitty images, valid only during the call.
GhosttyRenderState terminal_update(terminal* t, void (*images)(GhosttyTerminal terminal, GhosttyKittyGraphics graphics));
