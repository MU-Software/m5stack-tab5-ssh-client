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

static bool node(parser* p, layout_node* n, const cJSON* item, const layout_style* inherited, const char* size_key, int depth) {
    if (!cJSON_IsObject(item)) return fail(p, "a node is not an object");
    if (depth > MAX_DEPTH) return fail(p, "nodes nested deeper than %d", MAX_DEPTH);
    const cJSON *rows = field(item, "rows"), *columns = field(item, "columns"), *text = field(item, "text"), *button = field(item, "button"),
                *align = field(item, "align");
    n->style = *inherited;
    if (!style(p, item, &n->style) || !size(p, size_key != NULL ? field(item, size_key) : NULL, n)) return false;
    n->border = cJSON_IsTrue(field(item, "border"));
    if (text != NULL && !cJSON_IsString(text)) return fail(p, "text: not a string");
    n->text = text != NULL ? text->valuestring : "";
    if ((rows != NULL) + (columns != NULL) + (button != NULL) + (field(item, "terminal") != NULL) > 1)
        return fail(p, "a node with more than one of rows, columns, button and terminal");
    if (button != NULL) {
        if (!cJSON_IsString(button)) return fail(p, "button: not a string");
        n->kind = LAYOUT_BUTTON, n->action = button->valuestring, n->align = LAYOUT_CENTER;
    } else if (field(item, "terminal") != NULL) {
        n->kind = LAYOUT_TERMINAL;
    } else if (rows == NULL && columns == NULL) {
        if (text == NULL) return fail(p, "a node without rows, columns, text, button or terminal");
        n->kind = LAYOUT_TEXT;
    }
    if (align != NULL) {
        const char* a = cJSON_IsString(align) ? align->valuestring : "";
        if (strcmp(a, "left") != 0 && strcmp(a, "center") != 0 && strcmp(a, "right") != 0) return fail(p, "align %s: not left, center or right", a);
        n->align = a[0] == 'l' ? LAYOUT_LEFT : a[0] == 'c' ? LAYOUT_CENTER : LAYOUT_RIGHT;
    }
    if (rows == NULL && columns == NULL) return true;
    const cJSON* list = rows != NULL ? rows : columns;
    if (!cJSON_IsArray(list)) return fail(p, "%s: not a list", rows != NULL ? "rows" : "columns");
    int count = cJSON_GetArraySize(list);
    if (p->screen->count + (size_t)count > LAYOUT_MAX_NODES) return fail(p, "more than %d nodes", LAYOUT_MAX_NODES);
    n->kind = rows != NULL ? LAYOUT_ROWS : LAYOUT_COLUMNS;
    n->first = (uint16_t)p->screen->count, n->children = (uint16_t)count;
    p->screen->count += (size_t)count;
    for (int i = 0; i < count; i++)
        if (!node(p, &p->screen->nodes[n->first + i], cJSON_GetArrayItem(list, i), &n->style, rows != NULL ? "height" : "width", depth + 1))
            return false;
    return true;
}

layout* layout_parse(const char* text, const char* name, char* error, size_t size) {
    parser p = {calloc(1, sizeof(layout) + LAYOUT_MAX_NODES * sizeof(layout_node)), NULL, error, size};
    cJSON* json = cJSON_Parse(text);
    if (p.screen == NULL || json == NULL) {
        if (p.screen == NULL)
            fail(&p, "no memory");
        else
            fail(&p, "not JSON, near byte %zu", (size_t)(cJSON_GetErrorPtr() - text));
        free(p.screen);
        cJSON_Delete(json);
        return NULL;
    }
    p.screen->json = json, p.screen->count = 1, p.styles = field(json, "styles");
    const cJSON* screen = field(field(json, "screens"), name);
    layout_style none = {-1, -1, false};
    if (!(screen != NULL ? node(&p, &p.screen->nodes[0], screen, &none, NULL, 0) : fail(&p, "no screen %s", name))) {
        layout_free(p.screen);
        return NULL;
    }
    return p.screen;
}

void layout_free(layout* screen) {
    if (screen == NULL) return;
    cJSON_Delete(screen->json);
    free(screen);
}

static void place(layout* screen, layout_node* n, layout_area area) {
    static int32_t lengths[LAYOUT_MAX_NODES];
    n->area = n->inner = area;
    if (n->border && area.columns >= 2 && area.rows >= 2) n->inner = (layout_area){area.column + 1, area.row + 1, area.columns - 2, area.rows - 2};
    if (n->kind != LAYOUT_ROWS && n->kind != LAYOUT_COLUMNS) return;
    bool across = n->kind == LAYOUT_COLUMNS;
    int32_t length = across ? n->inner.columns : n->inner.rows, used = 0, shares = 0;
    layout_node* c = &screen->nodes[n->first];
    int32_t* l = &lengths[n->first];
    for (int i = 0; i < n->children; i++) {
        l[i] = c[i].unit == LAYOUT_CELLS ? c[i].size : c[i].unit == LAYOUT_PERCENT ? length * c[i].size / 100 : 0;
        if (l[i] > length - used) l[i] = length - used;
        used += l[i];
        if (c[i].unit == LAYOUT_SHARE) shares += c[i].size;
    }
    int32_t rest = length - used, left = rest;
    for (int i = 0; i < n->children; i++)
        if (c[i].unit == LAYOUT_SHARE && shares > 0) left -= l[i] = (int32_t)((int64_t)rest * c[i].size / shares);
    for (int i = 0; i < n->children && left > 0; i++)
        if (c[i].unit == LAYOUT_SHARE) l[i]++, left--;
    int32_t at = across ? n->inner.column : n->inner.row;
    for (int i = 0; i < n->children; at += l[i], i++)
        place(screen, &c[i],
              across ? (layout_area){at, n->inner.row, l[i], n->inner.rows} : (layout_area){n->inner.column, at, n->inner.columns, l[i]});
}

void layout_place(layout* screen, int32_t columns, int32_t rows) {
    place(screen, &screen->nodes[0], (layout_area){0, 0, columns, rows});
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
