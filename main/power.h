// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>

#include <esp_err.h>

// Charging at the full current, the C6 and the USB-A port powered, without resetting what a restart left running
esp_err_t power_start(void);
// The battery pack's voltage and current (the INA226 sits in the battery path, not the USB input) and whether USB-C is in
void power_report(void);
// The pack's voltage; false without the monitor
bool power_battery(int* millivolts);
