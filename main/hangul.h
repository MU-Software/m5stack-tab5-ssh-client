// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Dubeolsik indices, -1 (final: 0) for none
typedef struct {
    int8_t initial, medial;
    uint8_t final;
} hangul;

#define HANGUL_EMPTY ((hangul){-1, -1, 0})

// Upper case: with Shift. `out`: 3+ bytes, what the key completed
bool hangul_key(hangul* h, char letter, char* out, size_t* written);
bool hangul_backspace(hangul* h);
// `out`: 3+ bytes
size_t hangul_flush(hangul* h, char* out);
uint32_t hangul_preedit(const hangul* h);
