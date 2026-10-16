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

static const char* shown(const char* name, int n) {
    static char text[80];
    settings_text(settings_find(name), &(setting_value){.number = n}, text, sizeof text);
    return text;
}

int main(void) {
    global_state state = {.log = {.enabled = true, .file_kb = 1024, .keep_mb = 32, .keep_days = 14}, .timezone = "KST-9", .echo = {true, 50}};
    setting_value v;
    check(settings_find("echo.delay_ms") != NULL && settings_find("nothing") == NULL, "found by name");
    settings_get(&state, settings_find("echo.delay_ms"), &v);
    check(v.number == 50, "a number read from the state");
    check(set_number(&state, "echo.delay_ms", 120) && state.echo.delay_ms == 120, "a number written to the state");
    check(!set_number(&state, "echo.delay_ms", 1001) && state.echo.delay_ms == 120, "a number past its maximum refused");
    check(set_number(&state, "echo.enabled", 0) && !state.echo.enabled && !set_number(&state, "echo.enabled", 2), "a toggle: 0 or 1 only");
    check(set_number(&state, "log.session_level", 1) && state.log.quiet_sessions && !set_number(&state, "log.session_level", 2),
          "a choice kept in a bool: warn is quiet");
    check(set_number(&state, "screen.refresh", 1) && state.refresh == 1 && settings_find("screen.refresh")->restart, "refresh: an index, at restart");
    check(stepped("echo.delay_ms", 50, 1) == 60 && stepped("echo.delay_ms", 0, -1) == 0, "steps of 10 within 0..1000");
    check(stepped("log.file_kb", 1024, 1) == 2048 && stepped("log.file_kb", 16, -1) == 16 && stepped("log.file_kb", 65536, 1) == 65536,
          "step 0 doubles and halves within range");
    check(stepped("log.session_level", 1, 1) == 0 && stepped("log.session_level", 0, -1) == 1, "choices wrap");
    check(stepped("bars.top", 1, 1) == 0 && stepped("bars.top", 0, -1) == 1, "toggles flip either way");
    check(strcmp(shown("log.keep_mb", 32), "32 MB") == 0 && strcmp(shown("bars.top", 1), "켬") == 0 &&
              strcmp(shown("screen.refresh", 1), "29 Hz") == 0,
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

    FILE* f = fopen("main/ui.json", "rb");
    char text[16384];
    size_t n = f != NULL ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f != NULL) fclose(f);
    text[n] = 0;
    cJSON* json = cJSON_Parse(text);
    int unknown = 0, editors = 0;
    char error[160];
    for (const cJSON* s = json != NULL ? cJSON_GetObjectItemCaseSensitive(json, "screens")->child : NULL; s != NULL; s = s->next) {
        layout* l = layout_parse(json, s->string, error, sizeof error);
        for (size_t i = 0; l != NULL && i < l->count; i++) {
            const layout_node* node = &l->nodes[i];
            const char* name = node->kind > LAYOUT_LIST ? node->name : node->when;
            if (name == NULL || (node->kind <= LAYOUT_LIST && strncmp(name, "bars.", 5) != 0)) continue;
            editors++;
            if (settings_find(name) == NULL) printf("     main/ui.json %s: no setting %s\n", s->string, name), unknown++;
        }
        free(l);
    }
    check(json != NULL && editors > 0 && unknown == 0, "main/ui.json: every editor and bar names a setting");
    cJSON_Delete(json);
    return failures != 0;
}
