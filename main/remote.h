// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>

#include <esp_err.h>

#include "state.h"

// The control commands over SSH on port 22 while state->remote says so, for the keys in the SD card's
// .ssh/authorized_keys; nothing listens without that file. The host key is the SD card's
// .config/tab5-ssh/ssh_host_ed25519_key, made on first use. command: the app's control commands, true when handled
esp_err_t remote_start(const global_state* state, bool (*command)(const char* line));
