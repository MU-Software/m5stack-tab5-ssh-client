// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <esp_err.h>

#include "state.h"

// From boot: log lines also go to a RAM ring, which keeps the newest until logs_configure
esp_err_t logs_start(void);
// Files on the SD card as settings say, YYYY-MM-DD-NN.log (0000-00-00 before the date is known), or none
void logs_configure(const log_settings* settings);
