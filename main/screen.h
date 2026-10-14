// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>

#include "state.h"

// Shiroko's calls stay on the task that calls these
esp_err_t screen_start(const global_state* state);
// Draws what the terminal changed
void screen_draw(void);
// From the session: the keys it sent, which the screen shows before the host echoes them; NULL when the session ended
void screen_typed(const void* bytes, size_t size);
// From the session: what it waits for, shown with the time since `since` (a repeat keeps the first); NULL when nothing
void screen_waiting(const char* what, TickType_t since);
