// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "state.h"

typedef enum { SETTING_TOGGLE, SETTING_NUMBER, SETTING_CHOICE, SETTING_TEXT } setting_kind;

// A setting the UI edits, named by its key in the configuration file
typedef struct {
    const char* name;
    setting_kind kind;
    // Numbers: minimum..maximum by step, or doubling and halving with step 0
    int minimum, maximum, step;
    const char* unit;
    // Choices, NULL-ended
    const char* const* options;
    // In global_state: a bool or an int (a toggle, a number or a choice's index), or a char array of `size`
    size_t offset, size;
    // Takes effect at the next start
    bool restart;
} setting;

typedef struct {
    int number;
    char text[64];
} setting_value;

// NULL for none
const setting* settings_find(const char* name);
void settings_get(const global_state* state, const setting* s, setting_value* out);
// False for a value out of range
bool settings_set(global_state* state, const setting* s, const setting_value* value);
// One step up (direction 1) or down (-1), kept in range: toggles flip and choices wrap
void settings_step(const setting* s, setting_value* value, int direction);
// The value as shown: on/off as 켬/끔, numbers with their unit
void settings_text(const setting* s, const setting_value* value, char* out, size_t size);
