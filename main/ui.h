// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <shiroko/shiroko.h>

#include "layout.h"

typedef enum { UI_TOUCH_DOWN, UI_TOUCH_MOVE, UI_TOUCH_UP } ui_touch_phase;

// The text for {name}, NULL for none
typedef const char* (*ui_value)(const char* name, void* user);

// A tilemap layer at z over columns x rows cells, transparent where the screen has no background
shr_status ui_start(shr_context* context, shr_pl_res_bitmap_font* font, int32_t z, int32_t columns, int32_t rows);
// Lays out `screen` (kept by the caller, NULL for none) and draws it whole at the next ui_draw
void ui_show(layout* screen);
// Draws the texts that changed
void ui_draw(ui_value value, void* user);
// A touch on cell (column, row); lifted over the button it went down on, that button's action
const char* ui_touch(ui_touch_phase phase, int32_t column, int32_t row);
