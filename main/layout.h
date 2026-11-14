// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cJSON.h>

// JSON, in cells. "rows" stacks children sized by "height", "columns" by "width": cells, "N%" of the parent or "Nfr", a share
// of what the others leave (absent: "1fr"). "when": SETTING hides a node while it is off. Leaves: "text" ({name}), "button"
// (an action, labelled by "text"), "terminal", "list" (rows of "item" with {field}s; "action" for the chosen one; "buttons"
// end each row, "width" cells, their "text" and "when" naming the row's fields; "stripe": STYLE of the first and every
// second row) and the editors "toggle", "number", "choice" and "field" (also a value that is no setting; "secret": shown as *)
enum { LAYOUT_MAX_NODES = 256, LAYOUT_MAX_BUTTONS = 6 };

typedef enum {
    LAYOUT_ROWS,
    LAYOUT_COLUMNS,
    LAYOUT_TEXT,
    LAYOUT_BUTTON,
    LAYOUT_TERMINAL,
    LAYOUT_LIST,
    LAYOUT_TOGGLE,
    LAYOUT_NUMBER,
    LAYOUT_CHOICE,
    LAYOUT_FIELD
} layout_kind;
typedef enum { LAYOUT_LEFT, LAYOUT_CENTER, LAYOUT_RIGHT } layout_align;
typedef enum { LAYOUT_SHARE, LAYOUT_CELLS, LAYOUT_PERCENT } layout_unit;

typedef struct {
    int32_t column, row, columns, rows;
} layout_area;

// 0xRRGGBB, -1 for none: the parent's colour; no background leaves the cells transparent
typedef struct {
    int32_t foreground, background;
    bool bold;
} layout_style;

typedef struct {
    layout_kind kind;
    // name: the list or the setting; item: a list's item
    const char *text, *action, *name, *item, *when;
    layout_style style;
    layout_align align;
    // A background, -1 for none
    int32_t stripe;
    // per_item: one of a list's buttons, on each of its rows
    bool border, secret, per_item;
    layout_unit unit;
    int32_t size;
    // Children are nodes[first .. first + children - 1]
    uint16_t first, children;
    // area minus the border; empty while hidden
    layout_area area, inner;
} layout_node;

typedef struct {
    size_t count;
    layout_node nodes[]; // nodes[0] is the screen
} layout;

typedef bool (*layout_shown)(void* user, const char* name);

// Styles: {"styles": {NAME: {"foreground", "background": "#RRGGBB", "bold"}}}. Strings point into `screen` and `json`; free()
layout* layout_parse(const cJSON* json, const cJSON* screen, char* error, size_t size);
void layout_place(layout* screen, layout_area area, layout_shown shown, void* user);
// The innermost node
const layout_node* layout_at(const layout* screen, int32_t column, int32_t row);
const layout_node* layout_find(const layout* screen, layout_kind kind);
