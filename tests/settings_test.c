// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout.h"

static int failures;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

static bool set_number(global_state* state, const char* name, int n) {
    return settings_set(state, settings_find(name), &(setting_value){.number = n});
}

static int stepped(const char* name, int n, int direction) {
    setting_value v = {.number = n};
    settings_step(settings_find(name), &v, direction);
    return v.number;
}

static char refusals[160];

static void refused(const char* name) {
    snprintf(refusals + strlen(refusals), sizeof refusals - strlen(refusals), "%s ", name);
}

static const char* load(global_state* state, const char* json) {
    cJSON* config = cJSON_Parse(json);
    refusals[0] = 0;
    settings_load(state, config, refused);
    cJSON_Delete(config);
    return refusals;
}

static char* read_text(const char* path) {
    FILE* f = fopen(path, "rb");
    char* text = calloc(1, 65536);
    if (f != NULL) fread(text, 1, 65535, f), fclose(f);
    return text;
}

// The schema's property for a setting's name, with its type, range or choices
static int schema_matches(const cJSON* schema, const setting* s) {
    char path[64];
    const cJSON* p = schema;
    snprintf(path, sizeof path, "%s", s->name);
    for (char* key = strtok(path, "."); key != NULL && p != NULL; key = strtok(NULL, "."))
        p = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(p, "properties"), key);
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(p, "type");
    const cJSON* choices = cJSON_GetObjectItemCaseSensitive(p, "enum");
    if (s->kind == SETTING_TOGGLE) return cJSON_IsString(type) && strcmp(type->valuestring, "boolean") == 0;
    if (s->kind == SETTING_TEXT) return cJSON_IsString(type) && strcmp(type->valuestring, "string") == 0;
    if (s->kind == SETTING_NUMBER)
        return cJSON_IsString(type) && strcmp(type->valuestring, "integer") == 0 &&
               cJSON_GetObjectItemCaseSensitive(p, "minimum")->valueint == s->minimum &&
               cJSON_GetObjectItemCaseSensitive(p, "maximum")->valueint == s->maximum;
    int n = 0;
    for (; s->values[n] != NULL; n++)
        if (!cJSON_IsString(cJSON_GetArrayItem(choices, n)) || strcmp(cJSON_GetArrayItem(choices, n)->valuestring, s->values[n]) != 0) return 0;
    return cJSON_GetArraySize(choices) == n;
}

// Every setting of a and b the same
static int same(const global_state* a, const global_state* b) {
    setting_value x, y;
    for (size_t i = 0; settings_at(i) != NULL; i++) {
        settings_get(a, settings_at(i), &x), settings_get(b, settings_at(i), &y);
        if (x.number != y.number || strcmp(x.text, y.text) != 0) return 0;
    }
    return 1;
}

static const char* shown(const char* name, int n) {
    static char text[80];
    settings_text(settings_find(name), &(setting_value){.number = n}, text, sizeof text);
    return text;
}

int main(void) {
    global_state state = {.log = {.enabled = true, .file_kb = 1024, .keep_mb = 32, .keep_days = 14},
                          .timezone = "KST-9",
                          .echo = {true, 50},
                          .bars = {true, true},
                          .touch = {48, true}};
    setting_value v;
    check(settings_find("echo.delay_ms") != NULL && settings_find("nothing") == NULL, "found by name");
    settings_get(&state, settings_find("echo.delay_ms"), &v);
    check(v.number == 50, "a number read from the state");
    check(set_number(&state, "echo.delay_ms", 120) && state.echo.delay_ms == 120, "a number written to the state");
    check(!set_number(&state, "echo.delay_ms", 1001) && state.echo.delay_ms == 120, "a number past its maximum refused");
    check(set_number(&state, "echo.enabled", 0) && !state.echo.enabled && !set_number(&state, "echo.enabled", 2), "a toggle: 0 or 1 only");
    check(set_number(&state, "log.session_level", 1) && state.log.quiet_sessions && !set_number(&state, "log.session_level", 2),
          "a choice kept in a bool: warn is quiet");
    check(stepped("echo.delay_ms", 50, 1) == 60 && stepped("echo.delay_ms", 0, -1) == 0, "steps of 10 within 0..1000");
    check(stepped("log.file_kb", 1024, 1) == 2048 && stepped("log.file_kb", 16, -1) == 16 && stepped("log.file_kb", 65536, 1) == 65536,
          "step 0 doubles and halves within range");
    check(stepped("touch.wheel_px", 48, 1) == 60 && stepped("touch.wheel_px", 12, -1) == 12 && stepped("touch.wheel_px", 240, 1) == 240,
          "wheel pixels in cells of 12 within 12..240");
    check(stepped("log.session_level", 1, 1) == 0 && stepped("log.session_level", 0, -1) == 1, "choices wrap");
    check(stepped("bars.top", 1, 1) == 0 && stepped("bars.top", 0, -1) == 1, "toggles flip either way");
    check(strcmp(shown("log.keep_mb", 32), "32 MB") == 0 && strcmp(shown("bars.top", 1), "켬") == 0 &&
              strcmp(shown("bars.battery", 1), "퍼센트") == 0,
          "shown with units, 켬/끔 and choices");
    setting_value tz = {0};
    snprintf(tz.text, sizeof tz.text, "UTC0");
    check(settings_set(&state, settings_find("timezone"), &tz) && strcmp(state.timezone, "UTC0") == 0, "text written");
    memset(tz.text, 'A', 47);
    tz.text[47] = 0;
    check(settings_set(&state, settings_find("timezone"), &tz), "47 characters fit a 48-byte time zone");
    tz.text[47] = 'A', tz.text[48] = 0;
    check(!settings_set(&state, settings_find("timezone"), &tz) && strlen(state.timezone) == 47, "48 characters refused");
    check(!settings_set(&state, settings_find("timezone"), &(setting_value){0}), "an empty time zone refused");
    check(settings_set(&state, settings_find("remote.user"), &(setting_value){0}) && state.remote.user[0] == 0, "an empty user name: any");

    check(set_number(&state, "bars.top", 0) && !set_number(&state, "bars.bottom", 0) && state.bars.bottom, "both bars off refused");
    set_number(&state, "bars.top", 1);

    global_state loaded = state;
    check(*load(&loaded, "{}") == 0 && same(&loaded, &state), "an empty configuration keeps every value");
    check(*load(&loaded, "{\"log\": {\"enabled\": false, \"file_kb\": 256, \"session_level\": \"warn\"}, \"timezone\": \"UTC0\", "
                         "\"echo\": {\"delay_ms\": 200}, \"bars\": {\"battery\": \"percent\"}}") == 0 &&
              !loaded.log.enabled && loaded.log.file_kb == 256 && loaded.log.quiet_sessions && strcmp(loaded.timezone, "UTC0") == 0 &&
              loaded.echo.delay_ms == 200 && loaded.battery == 1,
          "settings read by their names, choices by their configuration values");
    check(strcmp(load(&loaded, "{\"log\": {\"file_kb\": 8, \"keep_days\": 1.5, \"keep_mb\": 4}, \"bars\": {\"battery\": \"%\"}}"),
                 "bars.battery log.file_kb log.keep_days ") == 0 &&
              loaded.log.file_kb == 256 && loaded.log.keep_mb == 4,
          "each value out of range, fractional or unknown refused alone");
    check(strcmp(load(&loaded, "{\"echo\": [], \"timezone\": 9, \"bars\": {\"top\": false, \"bottom\": false}}"),
                 "echo.enabled echo.delay_ms bars.bottom timezone ") == 0 &&
              !loaded.bars.top && loaded.bars.bottom,
          "a list for an object, a number for text and the last bar refused");

    loaded.bars.top = true;
    char* schema_text = read_text("tools/config.schema.json");
    cJSON* schema = cJSON_Parse(schema_text);
    cJSON* config = cJSON_CreateObject();
    int mismatched = 0;
    for (size_t i = 0; settings_at(i) != NULL; i++) {
        const setting* s = settings_at(i);
        if (!schema_matches(schema, s)) printf("     tools/config.schema.json: %s differs\n", s->name), mismatched++;
        char path[64];
        snprintf(path, sizeof path, "%s", s->name);
        char* dot = strchr(path, '.');
        cJSON* parent = config;
        if (dot != NULL) {
            *dot = 0;
            if ((parent = cJSON_GetObjectItemCaseSensitive(config, path)) == NULL) parent = cJSON_AddObjectToObject(config, path);
        }
        cJSON_AddItemToObject(parent, dot != NULL ? dot + 1 : path, settings_json(&loaded, s));
    }
    check(schema != NULL && mismatched == 0, "tools/config.schema.json: each setting with its type, range or choices");
    global_state again = state;
    refusals[0] = 0;
    settings_load(&again, config, refused);
    check(*refusals == 0 && same(&again, &loaded), "settings written as JSON read back the same");
    cJSON_Delete(config);
    cJSON_Delete(schema);
    free(schema_text);

    char* text = read_text("main/ui.json");
    cJSON* json = cJSON_Parse(text);
    int unknown = 0, editors = 0;
    char error[160];
    for (const cJSON* s = json != NULL ? cJSON_GetObjectItemCaseSensitive(json, "screens")->child : NULL; s != NULL; s = s->next) {
        layout* l = layout_parse(json, s->string, error, sizeof error);
        for (size_t i = 0; l != NULL && i < l->count; i++) {
            const layout_node* node = &l->nodes[i];
            const char* name = node->kind > LAYOUT_LIST ? node->name : node->when;
            if (name == NULL || (node->kind <= LAYOUT_LIST && strncmp(name, "bars.", 5) != 0) ||
                (node->kind == LAYOUT_FIELD && strncmp(name, "wifi.", 5) == 0))
                continue;
            editors++;
            if (settings_find(name) == NULL) printf("     main/ui.json %s: no setting %s\n", s->string, name), unknown++;
        }
        free(l);
    }
    check(json != NULL && editors > 0 && unknown == 0, "main/ui.json: every editor and bar names a setting, but the Wi-Fi screen's fields");
    cJSON_Delete(json);
    free(text);
    return failures != 0;
}
