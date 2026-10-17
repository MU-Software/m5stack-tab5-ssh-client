// SPDX-License-Identifier: GPL-3.0-or-later
#include "layout.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

enum { MAX_DEPTH = 16 };

typedef struct {
    layout* screen;
    const cJSON* styles;
    char* error;
    size_t size;
} parser;

static bool fail(parser* p, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(p->error, p->size, format, args);
    va_end(args);
    return false;
}

static const cJSON* field(const cJSON* object, const char* name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static bool color(parser* p, const cJSON* item, int32_t* out) {
    char* end;
    if (item == NULL) return true;
    if (!cJSON_IsString(item) || item->valuestring[0] != '#' || strlen(item->valuestring) != 7) return fail(p, "colour: not #RRGGBB");
    long value = strtol(item->valuestring + 1, &end, 16);
    if (*end != 0) return fail(p, "colour %s: not #RRGGBB", item->valuestring);
    *out = (int32_t)value;
    return true;
}

static bool style(parser* p, const cJSON* node, layout_style* out) {
    const cJSON* name = field(node, "style");
    if (name == NULL) return true;
    const cJSON* s = cJSON_IsString(name) ? field(p->styles, name->valuestring) : NULL;
    if (!cJSON_IsObject(s)) return fail(p, "no style %s", cJSON_IsString(name) ? name->valuestring : "(not a string)");
    const cJSON* bold = field(s, "bold");
    if (bold != NULL) out->bold = cJSON_IsTrue(bold);
    return color(p, field(s, "foreground"), &out->foreground) && color(p, field(s, "background"), &out->background);
}

static bool size(parser* p, const cJSON* item, layout_node* n) {
    char* end = NULL;
    long value = -1;
    n->unit = LAYOUT_SHARE, n->size = 1;
    if (item == NULL) return true;
    if (cJSON_IsNumber(item) && item->valuedouble == (int)item->valuedouble && item->valueint >= 0 && item->valueint <= 1000) {
        n->unit = LAYOUT_CELLS, n->size = item->valueint;
        return true;
    }
    if (cJSON_IsString(item)) value = strtol(item->valuestring, &end, 10);
    if (end != NULL && end != item->valuestring && value >= 0 && strcmp(end, "%") == 0 && value <= 100)
        n->unit = LAYOUT_PERCENT;
    else if (end != NULL && end != item->valuestring && value > 0 && strcmp(end, "fr") == 0 && value <= 1000)
        n->unit = LAYOUT_SHARE;
    else
        return fail(p, "size %s: not cells, N%% or Nfr", cJSON_IsString(item) ? item->valuestring : "(not a string)");
    n->size = (int32_t)value;
    return true;
}

// `key` as a string into `out`, which keeps its value when the key is absent
static bool string(parser* p, const cJSON* item, const char* key, const char** out) {
    const cJSON* v = field(item, key);
    if (v != NULL && !cJSON_IsString(v)) return fail(p, "%s: not a string", key);
    if (v != NULL) *out = v->valuestring;
    return true;
}

static const struct {
    const char* key;
    layout_kind kind;
} keys[] = {{"rows", LAYOUT_ROWS},     {"columns", LAYOUT_COLUMNS}, {"button", LAYOUT_BUTTON}, {"terminal", LAYOUT_TERMINAL}, {"list", LAYOUT_LIST},
            {"toggle", LAYOUT_TOGGLE}, {"number", LAYOUT_NUMBER},   {"choice", LAYOUT_CHOICE}, {"field", LAYOUT_FIELD}};

static bool node(parser* p, layout_node* n, const cJSON* item, const layout_style* inherited, const char* size_key, int depth) {
    if (!cJSON_IsObject(item)) return fail(p, "a node is not an object");
    if (depth > MAX_DEPTH) return fail(p, "nodes nested deeper than %d", MAX_DEPTH);
    const cJSON* list = NULL;
    const char* align = NULL;
    int kinds = 0;
    size_t found = 0;
    n->kind = LAYOUT_TEXT, n->text = NULL, n->item = "", n->style = *inherited;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (field(item, keys[i].key) != NULL) found = i, n->kind = keys[i].kind, kinds++;
    if (kinds > 1) return fail(p, "a node with more than one of rows, columns, button, terminal, list, toggle, number, choice and field");
    if (!style(p, item, &n->style) || !size(p, size_key != NULL ? field(item, size_key) : NULL, n) || !string(p, item, "text", &n->text) ||
        !string(p, item, "when", &n->when) || !string(p, item, "align", &align) || !string(p, item, "item", &n->item) ||
        !string(p, item, "action", &n->action))
        return false;
    n->border = cJSON_IsTrue(field(item, "border"));
    n->secret = cJSON_IsTrue(field(item, "secret"));
    if (kinds == 0 && n->text == NULL) return fail(p, "a node without rows, columns, text or a widget");
    if (n->text == NULL) n->text = "";
    if (n->kind != LAYOUT_TEXT && n->kind != LAYOUT_LIST && n->kind != LAYOUT_FIELD) n->align = LAYOUT_CENTER;
    if (align != NULL) {
        if (strcmp(align, "left") != 0 && strcmp(align, "center") != 0 && strcmp(align, "right") != 0)
            return fail(p, "align %s: not left, center or right", align);
        n->align = align[0] == 'l' ? LAYOUT_LEFT : align[0] == 'c' ? LAYOUT_CENTER : LAYOUT_RIGHT;
    }
    if (n->kind != LAYOUT_ROWS && n->kind != LAYOUT_COLUMNS) {
        list = n->kind == LAYOUT_TEXT || n->kind == LAYOUT_TERMINAL ? NULL : field(item, keys[found].key);
        if (list != NULL && !cJSON_IsString(list)) return fail(p, "%s: not a string", keys[found].key);
        if (n->kind == LAYOUT_BUTTON)
            n->action = list->valuestring;
        else if (list != NULL)
            n->name = list->valuestring;
        if (n->kind == LAYOUT_BUTTON && !string(p, item, "for", &n->name)) return false;
        return true;
    }
    list = field(item, keys[found].key);
    if (!cJSON_IsArray(list)) return fail(p, "%s: not a list", keys[found].key);
    int count = cJSON_GetArraySize(list);
    if (p->screen->count + (size_t)count > LAYOUT_MAX_NODES) return fail(p, "more than %d nodes", LAYOUT_MAX_NODES);
    n->first = (uint16_t)p->screen->count, n->children = (uint16_t)count;
    p->screen->count += (size_t)count;
    for (int i = 0; i < count; i++)
        if (!node(p, &p->screen->nodes[n->first + i], cJSON_GetArrayItem(list, i), &n->style, n->kind == LAYOUT_ROWS ? "height" : "width", depth + 1))
            return false;
    return true;
}

layout* layout_parse(const cJSON* json, const char* name, char* error, size_t size) {
    parser p = {calloc(1, sizeof(layout) + LAYOUT_MAX_NODES * sizeof(layout_node)), field(json, "styles"), error, size};
    const cJSON* screen = field(field(json, "screens"), name);
    layout_style none = {-1, -1, false};
    if (p.screen == NULL) {
        fail(&p, "no memory");
        return NULL;
    }
    p.screen->count = 1;
    if (!(screen != NULL ? node(&p, &p.screen->nodes[0], screen, &none, NULL, 0) : fail(&p, "no screen %s", name))) {
        free(p.screen);
        return NULL;
    }
    return p.screen;
}

static void place(layout* screen, layout_node* n, layout_area area, layout_shown shown, void* user) {
    static int32_t lengths[LAYOUT_MAX_NODES];
    n->area = n->inner = area;
    if (n->border && area.columns >= 2 && area.rows >= 2) n->inner = (layout_area){area.column + 1, area.row + 1, area.columns - 2, area.rows - 2};
    if (n->kind != LAYOUT_ROWS && n->kind != LAYOUT_COLUMNS) return;
    bool across = n->kind == LAYOUT_COLUMNS;
    int32_t length = across ? n->inner.columns : n->inner.rows, used = 0, shares = 0;
    layout_node* c = &screen->nodes[n->first];
    int32_t* l = &lengths[n->first];
    for (int i = 0; i < n->children; i++) {
        bool hidden = c[i].when != NULL && !shown(user, c[i].when);
        l[i] = hidden ? 0 : c[i].unit == LAYOUT_CELLS ? c[i].size : c[i].unit == LAYOUT_PERCENT ? length * c[i].size / 100 : -1;
        if (l[i] > length - used) l[i] = length - used;
        if (l[i] >= 0) used += l[i];
        if (l[i] < 0) shares += c[i].size;
    }
    int32_t rest = length - used, left = rest;
    for (int i = 0; i < n->children; i++)
        if (l[i] < 0 && shares > 0) left -= l[i] = (int32_t)((int64_t)rest * c[i].size / shares);
    for (int i = 0; i < n->children && left > 0; i++)
        if (c[i].unit == LAYOUT_SHARE && (c[i].when == NULL || shown(user, c[i].when))) l[i]++, left--;
    int32_t at = across ? n->inner.column : n->inner.row;
    for (int i = 0; i < n->children; at += l[i], i++)
        place(screen, &c[i],
              across ? (layout_area){at, n->inner.row, l[i], n->inner.rows} : (layout_area){n->inner.column, at, n->inner.columns, l[i]}, shown,
              user);
}

void layout_place(layout* screen, layout_area area, layout_shown shown, void* user) {
    place(screen, &screen->nodes[0], area, shown, user);
}

static bool inside(const layout_area* a, int32_t column, int32_t row) {
    return column >= a->column && column < a->column + a->columns && row >= a->row && row < a->row + a->rows;
}

const layout_node* layout_at(const layout* screen, int32_t column, int32_t row) {
    const layout_node* n = &screen->nodes[0];
    if (!inside(&n->area, column, row)) return NULL;
    for (int i = 0; i < n->children;)
        if (inside(&screen->nodes[n->first + i].area, column, row))
            n = &screen->nodes[n->first + i], i = 0;
        else
            i++;
    return n;
}

const layout_node* layout_find(const layout* screen, layout_kind kind) {
    for (size_t i = 0; i < screen->count; i++)
        if (screen->nodes[i].kind == kind) return &screen->nodes[i];
    return NULL;
}
