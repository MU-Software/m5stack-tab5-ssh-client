// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define FIELD(f) offsetof(global_state, f), sizeof(((global_state*)0)->f)

static const char* const levels[] = {"info", "warn", NULL};
static const char* const rates[] = {"57 Hz", "29 Hz", NULL};

static const setting table[] = {
    {"echo.enabled", SETTING_TOGGLE, .offset = FIELD(echo.enabled)},
    {"echo.delay_ms", SETTING_NUMBER, 0, 1000, 10, " ms", .offset = FIELD(echo.delay_ms)},
    {"bars.top", SETTING_TOGGLE, .offset = FIELD(bars.top)},
    {"bars.bottom", SETTING_TOGGLE, .offset = FIELD(bars.bottom)},
    {"screen.refresh", SETTING_CHOICE, .options = rates, .offset = FIELD(refresh), .restart = true},
    {"log.enabled", SETTING_TOGGLE, .offset = FIELD(log.enabled)},
    {"log.session_level", SETTING_CHOICE, .options = levels, .offset = FIELD(log.quiet_sessions)},
    {"log.file_kb", SETTING_NUMBER, 16, 65536, 0, " KB", .offset = FIELD(log.file_kb)},
    {"log.keep_mb", SETTING_NUMBER, 1, 65536, 0, " MB", .offset = FIELD(log.keep_mb)},
    {"log.keep_days", SETTING_NUMBER, 1, 3650, 1, "일", .offset = FIELD(log.keep_days)},
    {"timezone", SETTING_TEXT, .offset = FIELD(timezone)},
};

const setting* settings_find(const char* name) {
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcmp(table[i].name, name) == 0) return &table[i];
    return NULL;
}

static int options(const setting* s) {
    int n = 0;
    while (s->options[n] != NULL)
        n++;
    return n;
}

void settings_get(const global_state* state, const setting* s, setting_value* out) {
    const char* at = (const char*)state + s->offset;
    *out = (setting_value){0};
    if (s->kind == SETTING_TEXT)
        snprintf(out->text, sizeof out->text, "%s", at);
    else
        out->number = s->size == sizeof(bool) ? *(const bool*)at : *(const int*)at;
}

bool settings_set(global_state* state, const setting* s, const setting_value* value) {
    char* at = (char*)state + s->offset;
    int n = value->number;
    if (s->kind == SETTING_TEXT) {
        size_t length = strnlen(value->text, sizeof value->text);
        if (length == 0 || length >= s->size || length == sizeof value->text) return false;
        memcpy(at, value->text, length + 1);
        return true;
    }
    if ((s->kind == SETTING_TOGGLE && n != 0 && n != 1) || (s->kind == SETTING_NUMBER && (n < s->minimum || n > s->maximum)) ||
        (s->kind == SETTING_CHOICE && (n < 0 || n >= options(s))) || (s->size == sizeof(bool) && n != 0 && n != 1))
        return false;
    if (s->size == sizeof(bool))
        *(bool*)at = n != 0;
    else
        *(int*)at = n;
    return true;
}

void settings_step(const setting* s, setting_value* value, int direction) {
    int n = value->number;
    if (s->kind == SETTING_TOGGLE) {
        n = !n;
    } else if (s->kind == SETTING_CHOICE) {
        n = (n + direction + options(s)) % options(s);
    } else if (s->kind == SETTING_NUMBER) {
        n = s->step != 0 ? n + direction * s->step : direction > 0 ? n * 2 : n / 2;
        n = n < s->minimum ? s->minimum : n > s->maximum ? s->maximum : n;
    }
    value->number = n;
}

void settings_text(const setting* s, const setting_value* value, char* out, size_t size) {
    if (s->kind == SETTING_TOGGLE)
        snprintf(out, size, "%s", value->number ? "켬" : "끔");
    else if (s->kind == SETTING_NUMBER)
        snprintf(out, size, "%d%s", value->number, s->unit);
    else if (s->kind == SETTING_CHOICE)
        snprintf(out, size, "%s", value->number >= 0 && value->number < options(s) ? s->options[value->number] : "?");
    else
        snprintf(out, size, "%s", value->text);
}
