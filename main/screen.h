// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>

#include "state.h"
#include "terminal.h"
#include "ui.h"

// All on one task, as they call Shiroko: screen_start, screen_draw, screen_event, screen_resized, screen_show and screen_sleep
esp_err_t screen_start(const global_state* state);
// Before the first screen_draw
void screen_show(terminal* t);
void screen_draw(void);
bool screen_event(ui_event* out);
bool screen_resized(terminal_size* out);
// From any task. NULL closes; screen_panel_open counts one about to open; screen_input: since the last call
void screen_open(const char* panel);
bool screen_panel_open(void);
bool screen_input(void);
// esp_timer time, 0 before a touch
int64_t screen_touched_at(void);
esp_err_t screen_backlight(bool on);
// Waking, the next screen_draw draws the whole screen and turns the backlight on
esp_err_t screen_sleep(bool asleep);
bool screen_status(char* out, size_t size);
// From the session: the keys it sent; NULL when it ended
void screen_typed(const void* bytes, size_t size);
// From the session; a repeat keeps the first `since`; NULL when nothing
void screen_waiting(const char* what, TickType_t since);
