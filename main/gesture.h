// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

// A finger on the terminal, in pixels: a tap, or once it moved `slop` a drag that scrolls a line per `step` moved up or
// down. `step` may change between touches.
typedef struct {
    int32_t slop, step;
    bool down, dragging;
    int32_t start_x, start_y, lines;
} gesture;

// Where the finger went down; a tap there once it lifted, or lines to scroll now (up, into the past, negative)
typedef struct {
    int32_t x, y;
    bool tap;
    int32_t lines;
} gesture_event;

// The finger at (x, y), or lifted when !down
gesture_event gesture_touch(gesture* g, bool down, int32_t x, int32_t y);
