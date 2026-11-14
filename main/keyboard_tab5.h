// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <esp_err.h>
#include <stdint.h>

// Ext.Port1's G50, low while events wait
#define KEYBOARD_TAB5_INTERRUPT 50

// M5Stack's Keyboard for Tab5 (A164), attached at any time
esp_err_t keyboard_tab5_start(void);
esp_err_t keyboard_tab5_lights(uint8_t percent);
