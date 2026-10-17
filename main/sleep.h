// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

// The first two did not sleep: refused, or an SDIO transfer ran on
typedef enum { SLEEP_FAILED, SLEEP_BUSY, SLEEP_TIMER, SLEEP_KEYBOARD, SLEEP_USB, SLEEP_WIFI, SLEEP_OTHER } sleep_wake;

// Light sleep, with the screen already off, for up to `milliseconds` or until the Tab5 keyboard, a computer on USB-C or,
// with wifi, the C6's SDIO interrupt (experimental) wakes it
sleep_wake sleep_light(uint32_t milliseconds, bool wifi);
