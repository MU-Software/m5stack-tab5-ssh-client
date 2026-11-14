// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

// The first two did not sleep: refused, or an SDIO transfer ran on
typedef enum { SLEEP_FAILED, SLEEP_BUSY, SLEEP_TIMER, SLEEP_KEYBOARD, SLEEP_USB, SLEEP_WIFI, SLEEP_OTHER } sleep_wake;

// Screen off first. wifi: the C6's SDIO interrupt wakes it too (experimental)
sleep_wake sleep_light(uint32_t milliseconds, bool wifi);
// Counted, each hold released by the same code; no light sleep while held, nor before when
void sleep_hold(bool hold);
void sleep_hold_until(int64_t when);
bool sleep_held(int64_t now);
// Even while awake: +1 just before a light sleep and right after, before the ticks catch up. A wait that began and ended
// on one even value had no sleep
unsigned sleep_sequence(void);
