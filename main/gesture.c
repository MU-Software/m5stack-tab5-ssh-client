// SPDX-License-Identifier: GPL-3.0-or-later
#include "gesture.h"

#include <stdlib.h>

gesture_event gesture_touch(gesture* g, bool down, int32_t x, int32_t y) {
    if (down && !g->down) *g = (gesture){g->slop, g->step, true, false, x, y, 0};
    gesture_event e = {g->start_x, g->start_y, false, 0};
    if (!down) {
        e.tap = g->down && !g->dragging;
        g->down = false;
        return e;
    }
    g->dragging |= abs(x - g->start_x) >= g->slop || abs(y - g->start_y) >= g->slop;
    if (!g->dragging) return e;
    // The text follows the finger: down shows earlier lines
    int32_t lines = -(y - g->start_y) / g->step;
    e.lines = lines - g->lines;
    g->lines = lines;
    return e;
}
