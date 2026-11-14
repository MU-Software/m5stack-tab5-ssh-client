// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>

#include <esp_err.h>

#include "state.h"

// Keeps what a restart left running
esp_err_t power_start(void);
// The INA226 sits in the battery path, not the USB input
void power_report(void);
esp_err_t power_usb_a(bool on);
// Averages over 8.5 s instead of single samples
void power_average(bool on);
void power_pack(pack_reading* out);
