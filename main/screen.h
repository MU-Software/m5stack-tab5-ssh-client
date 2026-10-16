// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>

#include "state.h"
#include "ui.h"

// Shiroko's calls stay on the task that calls these: screen_start, screen_draw, screen_event and screen_resized
esp_err_t screen_start(const global_state* state);
// Draws what the terminal and the UI changed; touches, and keys while a panel is open, go to the UI
void screen_draw(void);
// What the UI asks for: settings to change, actions to take
bool screen_event(ui_event* out);
// The terminal's new size, once after screen_start and after each change of the layout
bool screen_resized(terminal_size* out);
// From any task: opens a panel (NULL closes), whether one is open (or about to), and whether the UI got a touch or a key
// since the last call
void screen_open(const char* panel);
bool screen_panel_open(void);
bool screen_input(void);
// What the session waits for and for how long ("held 3 s"); false when nothing
bool screen_status(char* out, size_t size);
// From the session: the keys it sent, which the screen shows before the host echoes them; NULL when the session ended
void screen_typed(const void* bytes, size_t size);
// From the session: what it waits for, shown with the time since `since` (a repeat keeps the first); NULL when nothing
void screen_waiting(const char* what, TickType_t since);
