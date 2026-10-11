// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <esp_err.h>

// A boot-protocol keyboard on the USB-A port, straight or not at all (no hubs)
esp_err_t keyboard_usb_start(void);
