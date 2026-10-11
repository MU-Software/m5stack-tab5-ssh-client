// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>
#include <ghostty/vt.h>

// A key of HID usage page 7; modifiers as in a boot report. Usage 0 released: every key.
typedef struct {
    uint8_t usage, modifiers;
    GhosttyKeyAction action;
    int64_t time;
} key_event;

esp_err_t keys_start(void);
// From any task; a press repeats until its release or another press
void keys_push(const key_event* event);
// From the SSH task: the next key as bytes for the channel, 0 when none is waiting. LANG1 or a short CapsLock switches
// between English and Korean (Dubeolsik), a long CapsLock locks capitals.
size_t keys_read(char* out, size_t size);
// The Korean syllable or jamo being typed, 0 for none
uint32_t keys_preedit(void);
