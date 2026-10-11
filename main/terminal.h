// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <esp_err.h>
#include <ghostty/vt.h>

#include "state.h"

// PSRAM; libghostty-vt has no allocator of its own without an OS
extern const GhosttyAllocator terminal_allocator;

esp_err_t terminal_start(const terminal_size* size);
// From the SSH channel; one task only, with terminal_reply
void terminal_write(const void* data, size_t size);
// The terminal's answers to queries, for the SSH channel
size_t terminal_reply(void* buffer, size_t size);
// From the render task: the terminal's state, safe to read until the next call
GhosttyRenderState terminal_update(void);
