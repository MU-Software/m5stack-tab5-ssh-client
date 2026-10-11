// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Dubeolsik composition: indices of the syllable being typed, -1 / 0 for none
typedef struct {
    int8_t initial, medial;
    uint8_t final;
} hangul;

#define HANGUL_EMPTY ((hangul){-1, -1, 0})

// A letter key (upper case: with Shift); false for other keys. `out` (3+ bytes) gets what the key completed.
bool hangul_key(hangul* h, char letter, char* out, size_t* written);
// Removes the last jamo typed; false when nothing is being composed
bool hangul_backspace(hangul* h);
// Completes what is being composed; returns its bytes in `out` (3+ bytes)
size_t hangul_flush(hangul* h, char* out);
// What is being composed, 0 for nothing
uint32_t hangul_preedit(const hangul* h);
