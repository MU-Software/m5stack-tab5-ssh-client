// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui.h"

#include <string.h>

enum { MAX_TEXT = 256 };
static shr_lyr* layer;
static int32_t columns, rows;
static layout* shown;
static bool whole;
static uint32_t drawn[LAYOUT_MAX_NODES]; // hashes of the texts as drawn
// The button a touch went down on, and whether it is still over it
static int pressed = -1;
static bool over;

shr_status ui_start(shr_context* context, shr_pl_res_bitmap_font* font, int32_t z, int32_t columns_count, int32_t rows_count) {
    columns = columns_count, rows = rows_count;
    shr_status status = shr_lyr_create(context, z, (shr_rect){0, 0, columns * SHR_CELL_WIDTH, rows * SHR_CELL_HEIGHT}, &layer);
    return status != SHR_OK ? status : shr_pl_lyr_tilemap_resize(layer, font, rows, columns, NULL);
}

void ui_show(layout* screen) {
    shown = screen, whole = true, pressed = -1;
    if (screen != NULL) layout_place(screen, columns, rows);
}

static shr_text_style look(const layout_style* s, bool inverse) {
    shr_color fg = s->foreground < 0 ? SHR_RGB(0xff, 0xff, 0xff) : 0xff000000u | (uint32_t)s->foreground;
    shr_color bg = s->background < 0 ? 0 : 0xff000000u | (uint32_t)s->background;
    if (inverse) {
        shr_color swap = fg;
        fg = bg != 0 ? bg : SHR_RGB(0, 0, 0), bg = swap;
    }
    return (shr_text_style){fg, bg, s->bold ? SHR_STYLE_BOLD : 0};
}

// `text` with each {name} replaced by its value
static size_t expand(const char* text, ui_value value, void* user, char* out) {
    size_t n = 0;
    while (*text != 0 && n < MAX_TEXT) {
        const char *end = text[0] == '{' ? strchr(text, '}') : NULL, *v = NULL;
        char name[32];
        if (end != NULL && (size_t)(end - text - 1) < sizeof name) {
            memcpy(name, text + 1, (size_t)(end - text - 1));
            name[end - text - 1] = 0;
            v = value(name, user);
            for (; v != NULL && *v != 0 && n < MAX_TEXT; v++)
                out[n++] = *v;
            text = end + 1;
        } else {
            out[n++] = *text++;
        }
    }
    out[n] = 0;
    return n;
}

static uint32_t hash(const char* text, size_t length, bool inverse) {
    uint32_t h = 2166136261u ^ inverse;
    for (size_t i = 0; i < length; i++)
        h = (h ^ (uint8_t)text[i]) * 16777619u;
    return h;
}

static void cell(int32_t row, int32_t column, const char* text, shr_text_style style) {
    shr_pl_lyr_tilemap_set_cell(layer, row, column, text, strlen(text), 1, style);
}

static void draw_border(const layout_node* n) {
    const layout_area* a = &n->area;
    shr_text_style s = look(&n->style, false);
    if (!n->border || a->columns < 2 || a->rows < 2) return;
    int32_t right = a->column + a->columns - 1, bottom = a->row + a->rows - 1;
    for (int32_t c = a->column + 1; c < right; c++)
        cell(a->row, c, "─", s), cell(bottom, c, "─", s);
    for (int32_t r = a->row + 1; r < bottom; r++)
        cell(r, a->column, "│", s), cell(r, right, "│", s);
    cell(a->row, a->column, "┌", s), cell(a->row, right, "┐", s), cell(bottom, a->column, "└", s), cell(bottom, right, "┘", s);
}

// On the middle row, cut with … to fit
static void draw_text(const layout_node* n, char* text, size_t length, bool inverse) {
    static shr_text_cluster clusters[MAX_TEXT];
    const layout_area* a = &n->inner;
    shr_text_style s = look(&n->style, inverse);
    shr_text_extent e = {0};
    if (a->columns <= 0 || a->rows <= 0) return;
    shr_pl_lyr_tilemap_clear(layer, a->row, a->column, a->rows, a->columns, s);
    if (shr_pl_lyr_tilemap_measure(text, length, a->columns, 0, clusters, MAX_TEXT, &e, NULL) != SHR_OK) return;
    int32_t width = e.cols;
    if (width > a->columns) {
        size_t i = 0;
        while (i < e.clusters && clusters[i].column + clusters[i].cells <= a->columns - 1)
            i++;
        length = clusters[i].byte_offset;
        memcpy(text + length, "…", 3);
        length += 3, width = clusters[i].column + 1;
    }
    int32_t left = n->align == LAYOUT_LEFT ? 0 : n->align == LAYOUT_CENTER ? (a->columns - width) / 2 : a->columns - width;
    shr_pl_lyr_tilemap_set_text(layer, a->row + (a->rows - 1) / 2, a->column + left, text, length, s, NULL, 0, 0, NULL);
}

void ui_draw(ui_value value, void* user) {
    char text[MAX_TEXT + 4];
    if (whole) shr_pl_lyr_tilemap_clear(layer, 0, 0, rows, columns, (shr_text_style){0});
    if (shown == NULL) {
        whole = false;
        return;
    }
    for (size_t i = 0; i < shown->count; i++) {
        const layout_node* n = &shown->nodes[i];
        if (whole && n->style.background >= 0)
            shr_pl_lyr_tilemap_clear(layer, n->area.row, n->area.column, n->area.rows, n->area.columns, look(&n->style, false));
        if (whole) draw_border(n);
        if (n->kind != LAYOUT_TEXT && n->kind != LAYOUT_BUTTON) continue;
        bool inverse = (int)i == pressed && over;
        size_t length = expand(n->text, value, user, text);
        uint32_t h = hash(text, length, inverse);
        if (!whole && h == drawn[i]) continue;
        drawn[i] = h;
        draw_text(n, text, length, inverse);
    }
    whole = false;
}

const char* ui_touch(ui_touch_phase phase, int32_t column, int32_t row) {
    const layout_node* n = shown != NULL ? layout_at(shown, column, row) : NULL;
    int at = n != NULL && n->kind == LAYOUT_BUTTON ? (int)(n - shown->nodes) : -1;
    if (phase == UI_TOUCH_DOWN) pressed = at;
    over = pressed >= 0 && at == pressed;
    if (phase != UI_TOUCH_UP) return NULL;
    const char* action = over ? shown->nodes[pressed].action : NULL;
    pressed = -1, over = false;
    return action;
}
