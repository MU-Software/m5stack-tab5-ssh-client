// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <esp_err.h>

// Boot-protocol keyboards only, no hubs
esp_err_t keyboard_usb_start(void);
