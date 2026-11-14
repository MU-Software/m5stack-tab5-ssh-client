// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>
#include <ghostty/vt.h>

// HID usage page 7, boot report modifiers; usage 0 released: every key
typedef struct {
    uint8_t usage, modifiers;
    GhosttyKeyAction action;
    int64_t time;
} key_event;

esp_err_t keys_start(void);
// From any task
void keys_push(const key_event* event);
// esp_timer time, 0 before any press
int64_t keys_pressed_at(void);
// From the SSH task
size_t keys_read(char* out, size_t size);
// Unicode, 0 for none
uint32_t keys_preedit(void);
// While on, keys_read leaves the keys to keys_take
void keys_divert(bool on);
// Presses and repeats; text: ASCII with Shift and CapsLock, 0 for none
bool keys_take(key_event* out, char* text);
