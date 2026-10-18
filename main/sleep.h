// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

// The first two did not sleep: refused, or an SDIO transfer ran on
typedef enum { SLEEP_FAILED, SLEEP_BUSY, SLEEP_TIMER, SLEEP_KEYBOARD, SLEEP_USB, SLEEP_WIFI, SLEEP_OTHER } sleep_wake;

// Light sleep, with the screen already off, for up to `milliseconds` or until the Tab5 keyboard, a computer on USB-C or,
// with wifi, the C6's SDIO interrupt (experimental) wakes it
sleep_wake sleep_light(uint32_t milliseconds, bool wifi);
// While held (counted, each hold released by the same code) the Tab5 does not light-sleep, nor before when
void sleep_hold(bool hold);
void sleep_hold_until(int64_t when);
bool sleep_held(int64_t now);
// Even while awake: raised just before a light sleep and again right after it, before the ticks catch up. A wait that began
// and ended at the same even value had no sleep inside
unsigned sleep_sequence(void);
