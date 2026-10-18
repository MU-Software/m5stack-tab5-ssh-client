// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cJSON.h>

// A screen in cells from JSON. "rows" stacks its children, each sized by "height"; "columns" sets them side by side,
// each sized by "width". A size is cells (a number), "N%" of the parent or "Nfr", a share of what the others leave
// (absent: "1fr"); "when": SETTING hides a node while that setting is off. Leaves: "text" (may hold {name}), "button"
// (an action, labelled by "text"), "terminal", "list" (a list's items, one a row, each as "item" with {field}s, and
// "action" for the chosen one) and the settings' editors "toggle", "number", "choice" and "field" (also a value of the
// screens' that is not a setting; "secret": shown as *).
enum { LAYOUT_MAX_NODES = 256 };

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

// 0xRRGGBB, -1 for none: a node without a colour takes its parent's, and no background leaves the cells transparent
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
    bool border, secret;
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

// Whether setting `name` is on
typedef bool (*layout_shown)(void* user, const char* name);

// A screen's node with the styles of {"styles": {NAME: {"foreground", "background": "#RRGGBB", "bold"}}}, its strings in
// `screen` and `json`; NULL with the reason in `error`. Free with free().
layout* layout_parse(const cJSON* json, const cJSON* screen, char* error, size_t size);
// Sets every node's area within `area`
void layout_place(layout* screen, layout_area area, layout_shown shown, void* user);
// The innermost node over cell (column, row), NULL outside the screen
const layout_node* layout_at(const layout* screen, int32_t column, int32_t row);
// The first node of `kind`, NULL if none
const layout_node* layout_find(const layout* screen, layout_kind kind);
