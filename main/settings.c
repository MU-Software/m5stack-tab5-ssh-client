// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define FIELD(f) offsetof(global_state, f), sizeof(((global_state*)0)->f)

static const char* const levels[] = {"info", "warn", NULL};
static const char *const batteries[] = {"전압", "퍼센트", NULL}, *const battery_values[] = {"volts", "percent", NULL};

static const setting table[] = {
    {"network.wifi.enabled", SETTING_TOGGLE, .offset = FIELD(wifi_enabled)},
    {"network.lte.enabled", SETTING_TOGGLE, .offset = FIELD(lte_enabled)},
    {"echo.enabled", SETTING_TOGGLE, .offset = FIELD(echo.enabled)},
    {"echo.delay_ms", SETTING_NUMBER, 0, 1000, 10, " ms", .offset = FIELD(echo.delay_ms)},
    {"bars.battery", SETTING_CHOICE, .options = batteries, .values = battery_values, .offset = FIELD(battery)},
    {"touch.wheel_px", SETTING_NUMBER, 12, 240, 12, " px", .offset = FIELD(touch.wheel_px)},
    {"touch.arrows", SETTING_TOGGLE, .offset = FIELD(touch.arrows)},
    {"sleep.idle_minutes", SETTING_NUMBER, 0, 60, 1, "분", .offset = FIELD(sleep.idle_minutes)},
    {"sleep.usb_a", SETTING_TOGGLE, .offset = FIELD(sleep.usb_a)},
    {"sleep.wifi_wake", SETTING_TOGGLE, .offset = FIELD(sleep.wifi_wake)},
    {"log.enabled", SETTING_TOGGLE, .offset = FIELD(log.enabled)},
    {"log.session_level", SETTING_CHOICE, .options = levels, .values = levels, .offset = FIELD(log.quiet_sessions)},
    {"log.file_kb", SETTING_NUMBER, 16, 65536, 0, " KB", .offset = FIELD(log.file_kb)},
    {"log.keep_mb", SETTING_NUMBER, 1, 65536, 0, " MB", .offset = FIELD(log.keep_mb)},
    {"log.keep_days", SETTING_NUMBER, 1, 3650, 1, "일", .offset = FIELD(log.keep_days)},
    {"timezone", SETTING_TEXT, 1, .offset = FIELD(timezone)},
    {"remote.enabled", SETTING_TOGGLE, .offset = FIELD(remote.enabled)},
    {"remote.lte", SETTING_TOGGLE, .offset = FIELD(remote.lte)},
    {"remote.user", SETTING_TEXT, .offset = FIELD(remote.user)},
};

const setting* settings_find(const char* name) {
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (strcmp(table[i].name, name) == 0) return &table[i];
    return NULL;
}

const setting* settings_at(size_t index) {
    return index < sizeof table / sizeof table[0] ? &table[index] : NULL;
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
        if (length < (size_t)s->minimum || length >= s->size || length == sizeof value->text) return false;
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

// "log.file_kb" in {"log": {"file_kb": …}}; NULL when absent, *wrong when something on the way is not an object
static const cJSON* lookup(const cJSON* config, const char* name, bool* wrong) {
    char key[32];
    const char* dot = strchr(name, '.');
    *wrong = !cJSON_IsObject(config);
    if (*wrong) return NULL;
    if (dot == NULL) return cJSON_GetObjectItemCaseSensitive(config, name);
    snprintf(key, sizeof key, "%.*s", (int)(dot - name), name);
    const cJSON* parent = cJSON_GetObjectItemCaseSensitive(config, key);
    return parent != NULL ? lookup(parent, dot + 1, wrong) : NULL;
}

bool settings_parse(const setting* s, const cJSON* item, setting_value* value) {
    *value = (setting_value){0};
    if (s->kind == SETTING_TOGGLE) {
        value->number = cJSON_IsTrue(item);
        return cJSON_IsBool(item);
    }
    if (s->kind == SETTING_NUMBER) {
        value->number = item->valueint;
        return cJSON_IsNumber(item) && item->valuedouble == item->valueint;
    }
    if (!cJSON_IsString(item)) return false;
    if (s->kind == SETTING_TEXT) return snprintf(value->text, sizeof value->text, "%s", item->valuestring) < (int)sizeof value->text;
    for (value->number = 0; s->values[value->number] != NULL; value->number++)
        if (strcmp(s->values[value->number], item->valuestring) == 0) return true;
    return false;
}

void settings_load(global_state* state, const cJSON* config, void (*refused)(const char* name)) {
    setting_value value;
    bool wrong;
    for (const setting* s = table; s < table + sizeof table / sizeof table[0]; s++) {
        const cJSON* item = lookup(config, s->name, &wrong);
        if ((item != NULL || wrong) && (wrong || !settings_parse(s, item, &value) || !settings_set(state, s, &value))) refused(s->name);
    }
}

cJSON* settings_json(const global_state* state, const setting* s) {
    setting_value value;
    settings_get(state, s, &value);
    if (s->kind == SETTING_TOGGLE) return cJSON_CreateBool(value.number);
    if (s->kind == SETTING_NUMBER) return cJSON_CreateNumber(value.number);
    return cJSON_CreateString(s->kind == SETTING_TEXT ? value.text : s->values[value.number]);
}
