// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <esp_err.h>
#include <stdint.h>

// Ext.Port1's G50, low while the keyboard has events
#define KEYBOARD_TAB5_INTERRUPT 50

// M5Stack's Keyboard for Tab5 (A164) on Ext.Port1, attached at any time, its LEDs off
esp_err_t keyboard_tab5_start(void);
// Both LEDs white at `percent` brightness, 0 for off
esp_err_t keyboard_tab5_lights(uint8_t percent);
