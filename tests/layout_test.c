// SPDX-License-Identifier: GPL-3.0-or-later
#include "layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static char error[160];
static cJSON* json;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

// Every setting is on but "off"
static bool shown(void* user, const char* name) {
    (void)user;
    return strcmp(name, "off") != 0;
}

// Screen `name` of `text`, which stays parsed until the next call
static layout* parse(const char* text, const char* name) {
    cJSON_Delete(json);
    json = cJSON_Parse(text);
    return layout_parse(json, name, error, sizeof error);
}

// The screen "s" of {"screens": {"s": NODE}}, laid out in columns x rows
static layout* screen(const char* node, int32_t columns, int32_t rows) {
    char text[1024];
    snprintf(text, sizeof text, "{\"styles\": {\"bar\": {\"background\": \"#102030\", \"bold\": true}}, \"screens\": {\"s\": %s}}", node);
    layout* l = parse(text, "s");
    if (l != NULL) layout_place(l, (layout_area){0, 0, columns, rows}, shown, NULL);
    return l;
}

static int widths(const layout* l, int a, int b, int c) {
    const layout_node* n = &l->nodes[1];
    return n[0].area.columns == a && n[1].area.columns == b && n[2].area.columns == c && n[1].area.column == a && n[2].area.column == a + b;
}

static int refused(const char* node, const char* reason) {
    layout* l = screen(node, 10, 10);
    free(l);
    return l == NULL && strstr(error, reason) != NULL;
}

int main(void) {
    layout* l = screen("{\"columns\": [{\"text\": \"a\"}, {\"text\": \"b\"}, {\"text\": \"c\"}]}", 106, 30);
    check(l != NULL && widths(l, 36, 35, 35) && l->nodes[1].area.rows == 30, "equal shares: the cells left over go to the first");
    free(l);
    l = screen("{\"columns\": [{\"width\": 10, \"text\": \"a\"}, {\"width\": \"50%\", \"text\": \"b\"}, {\"text\": \"c\"}]}", 100, 1);
    check(l != NULL && widths(l, 10, 50, 40), "cells, percent, and a share of the rest");
    free(l);
    l = screen("{\"columns\": [{\"width\": \"2fr\", \"text\": \"a\"}, {\"width\": \"1fr\", \"text\": \"b\"}, {\"width\": 0, \"text\": \"c\"}]}", 10,
               1);
    check(l != NULL && widths(l, 7, 3, 0), "weighted shares 2:1 of 10");
    free(l);
    l = screen("{\"columns\": [{\"width\": 80, \"text\": \"a\"}, {\"width\": 50, \"text\": \"b\"}, {\"text\": \"c\"}]}", 100, 1);
    check(l != NULL && widths(l, 80, 20, 0), "too wide: later nodes get what is left");
    free(l);
    l = screen(
        "{\"rows\": [{\"height\": 1, \"style\": \"bar\", \"columns\": [{\"text\": \"x\"}, {\"width\": 4, \"button\": \"go\", \"text\": \"Go\"}]}, "
        "{\"border\": true, \"rows\": [{\"terminal\": {}}]}]}",
        106, 30);
    const layout_node* t = l != NULL ? layout_find(l, LAYOUT_TERMINAL) : NULL;
    check(t != NULL && t->area.column == 1 && t->area.row == 2 && t->area.columns == 104 && t->area.rows == 27, "a terminal inside a border");
    const layout_node* b = l != NULL ? layout_at(l, 103, 0) : NULL;
    check(b != NULL && b->kind == LAYOUT_BUTTON && strcmp(b->action, "go") == 0 && b->align == LAYOUT_CENTER, "the innermost node: a button");
    check(b != NULL && b->style.background == 0x102030 && b->style.bold && b->style.foreground == -1, "a style passes to the children");
    check(l != NULL && layout_at(l, 106, 0) == NULL && layout_at(l, 0, 1)->kind == LAYOUT_ROWS, "outside the screen, and on a border");
    free(l);
    l = screen("{\"rows\": [{\"height\": \"50%\", \"columns\": [{\"rows\": [{\"text\": \"a\"}, {\"text\": \"b\"}]}, {\"text\": \"c\"}]}, {\"text\": "
               "\"d\"}]}",
               20, 9);
    b = l != NULL ? layout_at(l, 0, 3) : NULL;
    check(b != NULL && strcmp(b->text, "b") == 0 && b->area.row == 2 && b->area.rows == 2 && b->area.columns == 10, "rows in columns in rows");
    free(l);
    l = screen(
        "{\"rows\": [{\"height\": 1, \"when\": \"off\", \"text\": \"a\"}, {\"terminal\": {}}, {\"height\": 1, \"when\": \"on\", \"text\": \"b\"}]}",
        10, 30);
    t = l != NULL ? layout_find(l, LAYOUT_TERMINAL) : NULL;
    check(t != NULL && t->area.row == 0 && t->area.rows == 29 && l->nodes[1].area.rows == 0, "a node whose setting is off takes no cells");
    free(l);
    l = screen("{\"rows\": [{\"toggle\": \"echo.enabled\"}, {\"list\": \"hosts\", \"item\": \"{alias}\", \"action\": \"connect\"}, {\"field\": "
               "\"timezone\"}, {\"field\": \"wifi.password\", \"secret\": true}]}",
               10, 4);
    const layout_node* w = l != NULL ? &l->nodes[1] : NULL;
    check(w != NULL && w[0].kind == LAYOUT_TOGGLE && strcmp(w[0].name, "echo.enabled") == 0 && w[0].align == LAYOUT_CENTER, "a toggle");
    check(w != NULL && w[1].kind == LAYOUT_LIST && strcmp(w[1].name, "hosts") == 0 && strcmp(w[1].item, "{alias}") == 0 &&
              strcmp(w[1].action, "connect") == 0 && w[1].align == LAYOUT_LEFT,
          "a list with its item and action");
    check(w != NULL && w[2].kind == LAYOUT_FIELD && strcmp(w[2].name, "timezone") == 0 && w[2].align == LAYOUT_LEFT && !w[2].secret, "a field");
    check(w != NULL && w[3].kind == LAYOUT_FIELD && w[3].secret, "a secret field");
    free(l);
    l = screen("{\"rows\": [{\"button\": \"session end\", \"for\": \"sessions\", \"text\": \"끊기\"}, {\"button\": \"close\", \"text\": \"닫기\"}]}",
               10, 2);
    check(l != NULL && strcmp(l->nodes[1].action, "session end") == 0 && strcmp(l->nodes[1].name, "sessions") == 0 && l->nodes[2].name == NULL,
          "a button for a list");
    free(l);

    check(refused("{\"columns\": [{\"width\": \"3x\", \"text\": \"a\"}]}", "size 3x"), "a size without a unit refused");
    check(refused("{\"columns\": [{\"width\": \"120%\", \"text\": \"a\"}]}", "size"), "more than 100% refused");
    check(refused("{\"style\": \"none\", \"text\": \"a\"}", "no style none"), "an unknown style refused");
    check(refused("{\"rows\": [], \"button\": \"x\"}", "more than one"), "rows and a button in one node refused");
    check(refused("{\"rows\": [{}]}", "without rows"), "an empty node refused");
    check(refused("{\"text\": \"a\", \"align\": \"middle\"}", "align middle"), "an unknown alignment refused");
    check(refused("{\"toggle\": \"a\", \"button\": \"b\"}", "more than one"), "a toggle and a button in one node refused");
    check(refused("{\"toggle\": 1}", "toggle: not a string"), "a toggle without a setting name refused");
    char deep[1024];
    int at = 0;
    for (int i = 0; i < 20; i++)
        at += snprintf(deep + at, sizeof deep - (size_t)at, "{\"rows\": [");
    at += snprintf(deep + at, sizeof deep - (size_t)at, "{\"text\": \"a\"}");
    for (int i = 0; i < 20; i++)
        at += snprintf(deep + at, sizeof deep - (size_t)at, "]}");
    check(refused(deep, "deeper"), "nesting past the limit refused");
    check(parse("{\"screens\": {}}", "s") == NULL && strstr(error, "no screen s") != NULL, "a missing screen refused");

    FILE* f = fopen("main/ui.json", "rb");
    char text[16384];
    size_t n = f != NULL ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f != NULL) fclose(f);
    text[n] = 0;
    l = parse(text, "main");
    if (l == NULL) printf("     main/ui.json: %s\n", error);
    if (l != NULL) layout_place(l, (layout_area){0, 0, 106, 30}, shown, NULL);
    t = l != NULL ? layout_find(l, LAYOUT_TERMINAL) : NULL;
    check(t != NULL && t->area.row == 1 && t->area.columns == 106 && t->area.rows == 28, "main/ui.json: a 106 x 28 terminal between the bars");
    free(l);
    int screens = 0, parsed = 0;
    for (const cJSON* s = cJSON_GetObjectItemCaseSensitive(json, "screens")->child; s != NULL; s = s->next) {
        layout* p = layout_parse(json, s->string, error, sizeof error);
        if (p == NULL) printf("     main/ui.json %s: %s\n", s->string, error);
        screens++, parsed += p != NULL;
        free(p);
    }
    check(screens >= 4 && parsed == screens, "main/ui.json: every screen parses");
    cJSON_Delete(json);
    return failures != 0;
}
