// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include <cJSON.h>

#include "state.h"

typedef enum { SETTING_TOGGLE, SETTING_NUMBER, SETTING_CHOICE, SETTING_TEXT } setting_kind;

// name: its key in the configuration file
typedef struct {
    const char* name;
    setting_kind kind;
    // step 0: doubling and halving; text: at least minimum bytes
    int minimum, maximum, step;
    const char* unit;
    // NULL-ended: as shown, and in the configuration file
    const char *const *options, *const *values;
    // A number's 0 as shown, NULL for the number
    const char* zero;
    // In global_state: a bool (toggle), an int (number, choice index) or a char[size]
    size_t offset, size;
} setting;

typedef struct {
    int number;
    // A Wi-Fi key of 64 hex digits
    char text[65];
} setting_value;

const setting* settings_find(const char* name);
const setting* settings_at(size_t index);
void settings_get(const global_state* state, const setting* s, setting_value* out);
bool settings_set(global_state* state, const setting* s, const setting_value* value);
// direction: 1 or -1
void settings_step(const setting* s, setting_value* value, int direction);
void settings_text(const setting* s, const setting_value* value, char* out, size_t size);
bool settings_parse(const setting* s, const cJSON* item, setting_value* value);
// An absent setting keeps its value
void settings_load(global_state* state, const cJSON* config, void (*refused)(const char* name));
cJSON* settings_json(const global_state* state, const setting* s);
