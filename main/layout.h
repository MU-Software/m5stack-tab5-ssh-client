// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A screen in cells from JSON. "rows" stacks its children, each sized by "height"; "columns" sets them side by side,
// each sized by "width". A size is cells (a number), "N%" of the parent or "Nfr", a share of what the others leave
// (absent: "1fr"). Leaves: "text" (may hold {name}), "button" (an action, labelled by "text") and "terminal".
enum { LAYOUT_MAX_NODES = 256 };

typedef enum { LAYOUT_ROWS, LAYOUT_COLUMNS, LAYOUT_TEXT, LAYOUT_BUTTON, LAYOUT_TERMINAL } layout_kind;
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
    const char *text, *action;
    layout_style style;
    layout_align align;
    bool border;
    layout_unit unit;
    int32_t size;
    // Children are nodes[first .. first + children - 1]
    uint16_t first, children;
    // area minus the border
    layout_area area, inner;
} layout_node;

typedef struct {
    void* json; // holds the strings
    size_t count;
    layout_node nodes[]; // nodes[0] is the screen
} layout;

// Screen `name` of {"styles": {NAME: {"foreground", "background": "#RRGGBB", "bold"}}, "screens": {NAME: node}};
// NULL with the reason in `error`. Free with layout_free.
layout* layout_parse(const char* text, const char* name, char* error, size_t size);
void layout_free(layout* screen);
// Sets every node's area within columns x rows cells
void layout_place(layout* screen, int32_t columns, int32_t rows);
// The innermost node over cell (column, row), NULL outside the screen
const layout_node* layout_at(const layout* screen, int32_t column, int32_t row);
// The first node of `kind`, NULL if none
const layout_node* layout_find(const layout* screen, layout_kind kind);
