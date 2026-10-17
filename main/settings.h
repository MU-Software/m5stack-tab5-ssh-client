// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include <cJSON.h>

#include "state.h"

typedef enum { SETTING_TOGGLE, SETTING_NUMBER, SETTING_CHOICE, SETTING_TEXT } setting_kind;

// A setting the UI edits, named by its key in the configuration file
typedef struct {
    const char* name;
    setting_kind kind;
    // Numbers: minimum..maximum by step, or doubling and halving with step 0; text: at least minimum bytes
    int minimum, maximum, step;
    const char* unit;
    // Choices, NULL-ended, as shown and in the configuration file
    const char *const *options, *const *values;
    // In global_state: a bool or an int (a toggle, a number or a choice's index), or a char array of `size`
    size_t offset, size;
} setting;

typedef struct {
    int number;
    // Room for a Wi-Fi key of 64 hex digits
    char text[65];
} setting_value;

// NULL for none
const setting* settings_find(const char* name);
// NULL past the last
const setting* settings_at(size_t index);
void settings_get(const global_state* state, const setting* s, setting_value* out);
// False for a value out of range, or both bars off
bool settings_set(global_state* state, const setting* s, const setting_value* value);
// One step up (direction 1) or down (-1), kept in range: toggles flip and choices wrap
void settings_step(const setting* s, setting_value* value, int direction);
// The value as shown: on/off as 켬/끔, numbers with their unit
void settings_text(const setting* s, const setting_value* value, char* out, size_t size);
// A value as the configuration file keeps it; false when it is not of the setting's kind
bool settings_parse(const setting* s, const cJSON* item, setting_value* value);
// The configuration's settings into the state; each one absent keeps its value, each one refused is passed to refused
void settings_load(global_state* state, const cJSON* config, void (*refused)(const char* name));
// The value as the configuration file keeps it
cJSON* settings_json(const global_state* state, const setting* s);
