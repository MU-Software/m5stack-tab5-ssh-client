// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>

#include <esp_err.h>

#include "state.h"

// From boot: log lines also go to a RAM ring, which keeps the newest until logs_configure
esp_err_t logs_start(void);
// Files on the SD card as settings say, YYYY-MM-DD-NN.log (0000-00-00 before the date is known), or none
void logs_configure(const log_settings* settings);
// Another reader of the ring, which leaves the lines to the SD card: from logs_position(), or 0 for the oldest kept
size_t logs_position(void);
size_t logs_read(size_t* position, char* out, size_t max);
