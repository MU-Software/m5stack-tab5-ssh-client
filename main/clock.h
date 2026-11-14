// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>

#include <esp_err.h>

// After esp_netif_init
esp_err_t clock_start(void);
// POSIX TZ, such as "KST-9"
void clock_timezone(const char* timezone);
// The system time is a date, not time since boot
bool clock_known(void);
