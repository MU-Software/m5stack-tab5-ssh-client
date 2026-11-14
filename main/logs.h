// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>

#include <esp_err.h>

#include "state.h"

// From boot: the RAM ring keeps the newest lines until logs_configure
esp_err_t logs_start(void);
void logs_configure(const log_settings* settings);
// A reader that leaves the lines to the SD card: position from logs_position(), or 0 for the oldest kept
size_t logs_position(void);
size_t logs_read(size_t* position, char* out, size_t max);
