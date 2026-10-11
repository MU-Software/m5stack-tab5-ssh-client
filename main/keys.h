// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>
#include <ghostty/vt.h>

// A key of HID usage page 7; modifiers as in a boot report
typedef struct {
    uint8_t usage, modifiers;
    bool caps_lock;
    GhosttyKeyAction action;
} key_event;

esp_err_t keys_start(void);
// From any task
void keys_push(const key_event* event);
// From the SSH task: the next key as bytes for the channel, 0 when none is waiting
size_t keys_read(char* out, size_t size);
