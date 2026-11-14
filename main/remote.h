// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>

#include <esp_err.h>

#include "state.h"

// Listens while state->remote says so and .ssh/authorized_keys exists. command: the app's control commands, -1 for none of them
esp_err_t remote_start(const global_state* state, int (*command)(const char* line));
