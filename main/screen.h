// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <esp_err.h>

#include "state.h"

// Shiroko's calls stay on the task that calls these
esp_err_t screen_start(const global_state* state);
// Draws what the terminal changed
void screen_draw(void);
