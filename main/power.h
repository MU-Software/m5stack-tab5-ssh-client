// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <esp_err.h>

// Charging on, quick charge off: with a full battery the whole board already draws through the USB-C input, which runs hot
esp_err_t power_start(void);
