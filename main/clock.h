// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>

#include <esp_err.h>

// The time from the RTC (RX8130CE) when it holds a plausible one, then SNTP once there is a network, which also sets the
// RTC; after esp_netif_init
esp_err_t clock_start(void);
// A POSIX TZ string such as "KST-9", for local times
void clock_timezone(const char* timezone);
// The system time is the date, not the time since boot
bool clock_known(void);
