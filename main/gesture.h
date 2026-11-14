// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Pixels; `step` may change between touches
typedef struct {
    int32_t slop, step;
    bool down, dragging;
    int32_t start_x, start_y, lines;
} gesture;

// x, y: where the finger went down; lines < 0: into the past
typedef struct {
    int32_t x, y;
    bool tap;
    int32_t lines;
} gesture_event;

gesture_event gesture_touch(gesture* g, bool down, int32_t x, int32_t y);
