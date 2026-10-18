// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>

#include <esp_err.h>

#include "state.h"

// Charging at the full current, the C6 and the USB-A port powered, without resetting what a restart left running
esp_err_t power_start(void);
// The battery pack's voltage and current (the INA226 sits in the battery path, not the USB input), whether it is charging and the
// charger's LED line
void power_report(void);
esp_err_t power_usb_a(bool on);
// The INA226's readings as averages over 8.5 s instead of single samples
void power_average(bool on);
// out->known false without the monitor
void power_pack(pack_reading* out);
