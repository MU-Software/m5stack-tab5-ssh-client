// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <esp_err.h>
#include <ghostty/vt.h>

#include "state.h"

// PSRAM; libghostty-vt has no allocator of its own without an OS
extern const GhosttyAllocator terminal_allocator;

// Written by its session's task, drawn by the screen's
typedef struct terminal terminal;

terminal* terminal_new(const terminal_size* size);
void terminal_free(terminal* t);
void terminal_resize(terminal* t, const terminal_size* size);
// The terminal keys are encoded for
void terminal_show(terminal* t);
// One task only, with terminal_reply
void terminal_write(terminal* t, const void* data, size_t size);
size_t terminal_reply(terminal* t, void* buffer, size_t size);
// As the shown terminal's modes want them
size_t terminal_encode_key(GhosttyKeyEvent event, char* out, size_t size);
// From the render task: pixels in t, `lines` up negative. Without mouse reports the wheel sends Up/Down on the alternate
// screen if `arrows`, else scrolls t's scrollback until terminal_scroll_to_bottom
void terminal_click(terminal* t, int32_t x, int32_t y);
void terminal_wheel(terminal* t, int32_t lines, int32_t x, int32_t y, bool arrows);
void terminal_scroll_to_bottom(terminal* t);
// The wheel's keys too
size_t terminal_read_mouse(terminal* t, void* buffer, size_t size);
// From the render task: valid until the next call; replay its row moves first, as moved rows are not dirty. The pixels
// `images` gets live only during the call. NULL: out of memory with the state half made; the next update redraws in full
GhosttyRenderState terminal_update(terminal* t, void (*images)(GhosttyRenderState render, GhosttyKittyGraphics graphics));
