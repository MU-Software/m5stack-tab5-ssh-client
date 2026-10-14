// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <esp_err.h>

// Charging on, quick charge off: with a full battery the whole board already draws through the USB-C input, which runs hot
esp_err_t power_start(void);
// The battery pack's voltage and current (the INA226 sits in the battery path, not the USB input) and the charger's status line
void power_report(void);
