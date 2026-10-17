// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_TEXT = 256, MAX_SCREENS = 8 };

typedef struct {
    layout* screen;
    uint32_t drawn[LAYOUT_MAX_NODES]; // hashes of what each node shows
} shown_screen;

static shr_lyr *layer, *edge;
static int32_t columns, rows, width;
static const ui_source* source;
static cJSON* json;
static struct {
    const char* name;
    layout* screen;
} screens[MAX_SCREENS];
static size_t screen_count;
static shown_screen main_screen, panel;
static char panel_name[32];
static bool whole;
static uint32_t laid_out; // hash of the "when"s
// The node a touch went down on, and whether it is still over it
static const layout_node* pressed;
static bool over;
// In the panel: the node keys go to, the item chosen and the first shown of each list, a field being edited and whether
// anything was typed into it
static int focus = -1;
static size_t chosen[LAYOUT_MAX_NODES], first[LAYOUT_MAX_NODES];
static bool editing, edited;
static char edit[sizeof((setting_value*)0)->text];

shr_status ui_start(shr_context* context, shr_pl_res_bitmap_font* font, int32_t z, int32_t columns_count, int32_t rows_count, int32_t width_pixels,
                    const ui_source* values) {
    columns = columns_count, rows = rows_count, width = width_pixels, source = values;
    shr_status status = shr_lyr_create(context, z, (shr_rect){0, 0, columns * SHR_CELL_WIDTH, rows * SHR_CELL_HEIGHT}, &layer);
    if (status == SHR_OK) status = shr_pl_lyr_tilemap_resize(layer, font, rows, columns, NULL);
    if (status == SHR_OK && width > columns * SHR_CELL_WIDTH)
        status = shr_lyr_create(context, z, (shr_rect){columns * SHR_CELL_WIDTH, 0, width, rows * SHR_CELL_HEIGHT}, &edge);
    return status;
}

static bool shown(void* user, const char* name) {
    (void)user;
    const setting* s = settings_find(name);
    setting_value v;
    if (s == NULL) {
        const char* text = source->text(source->user, name);
        return text != NULL && text[0] != 0;
    }
    source->get(source->user, s, &v);
    return v.number != 0;
}

static uint32_t mix(uint32_t h, const void* bytes, size_t size) {
    for (size_t i = 0; i < size; i++)
        h = (h ^ ((const uint8_t*)bytes)[i]) * 16777619u;
    return h;
}

static uint32_t whens(void) {
    uint32_t h = 2166136261u;
    const layout* all[] = {main_screen.screen, panel.screen};
    for (size_t k = 0; k < 2; k++)
        for (size_t i = 0; all[k] != NULL && i < all[k]->count; i++)
            if (all[k]->nodes[i].when != NULL) h = mix(h, (uint8_t[]){shown(NULL, all[k]->nodes[i].when)}, 1);
    return h;
}

static void lay_out(void) {
    layout_area all = {0, 0, columns, rows};
    const layout_node* t = NULL;
    if (main_screen.screen != NULL) {
        layout_place(main_screen.screen, all, shown, NULL);
        t = layout_find(main_screen.screen, LAYOUT_TERMINAL);
    }
    if (panel.screen != NULL) layout_place(panel.screen, t != NULL ? t->area : all, shown, NULL);
    laid_out = whens(), whole = true;
}

static layout* find(const char* name) {
    for (size_t i = 0; i < screen_count; i++)
        if (strcmp(screens[i].name, name) == 0) return screens[i].screen;
    return NULL;
}

// Lists without an action only show
static bool focusable(const layout_node* n) {
    return n->kind > LAYOUT_LIST || n->kind == LAYOUT_BUTTON || (n->kind == LAYOUT_LIST && n->action != NULL);
}

static bool visible(const layout_node* n) {
    return n->area.columns > 0 && n->area.rows > 0;
}

static void move_focus(int direction) {
    for (size_t step = 0; panel.screen != NULL && step < panel.screen->count; step++) {
        focus = (focus + direction + (int)panel.screen->count) % (int)panel.screen->count;
        const layout_node* n = &panel.screen->nodes[focus];
        if (focusable(n) && visible(n)) return;
    }
}

bool ui_open(const char* name) {
    layout* screen = name != NULL ? find(name) : NULL;
    if (name != NULL && screen == NULL) return false;
    panel.screen = screen, focus = -1, editing = false, pressed = NULL;
    memset(edit, 0, sizeof edit);
    snprintf(panel_name, sizeof panel_name, "%s", name != NULL ? name : "");
    lay_out();
    for (size_t i = 0; screen != NULL && i < screen->count; i++) {
        const layout_node* n = &screen->nodes[i];
        chosen[i] = n->kind == LAYOUT_LIST && source->chosen != NULL ? source->chosen(source->user, n->name) : 0, first[i] = 0;
        // A list or an editor first, else a button that does more than close
        if (focusable(n) && visible(n) && (focus < 0 || (screen->nodes[focus].kind == LAYOUT_BUTTON && n->kind != LAYOUT_BUTTON)) &&
            (n->kind != LAYOUT_BUTTON || (focus < 0 && strcmp(n->action, "close") != 0)))
            focus = (int)i;
    }
    return true;
}

const char* ui_panel(void) {
    return panel.screen != NULL ? panel_name : NULL;
}

bool ui_load(const char* text, char* error, size_t size) {
    cJSON* next = cJSON_Parse(text);
    const cJSON* screen;
    size_t count = 0;
    layout* parsed[MAX_SCREENS];
    if (next == NULL) {
        snprintf(error, size, "not JSON, near byte %zu", (size_t)(cJSON_GetErrorPtr() - text));
        return false;
    }
    bool ok = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(next, "screens"), "main") != NULL;
    if (!ok) snprintf(error, size, "no screen main");
    cJSON_ArrayForEach(screen, cJSON_GetObjectItemCaseSensitive(next, "screens")) {
        if (!ok) break;
        if (count == MAX_SCREENS) {
            snprintf(error, size, "more than %d screens", MAX_SCREENS);
            ok = false;
        } else if ((parsed[count] = layout_parse(next, screen->string, error, size)) == NULL) {
            ok = false;
        } else {
            count++;
        }
    }
    if (!ok) {
        while (count > 0)
            free(parsed[--count]);
        cJSON_Delete(next);
        return false;
    }
    for (size_t i = 0; i < screen_count; i++)
        free(screens[i].screen);
    cJSON_Delete(json);
    json = next, screen_count = 0;
    cJSON_ArrayForEach(screen, cJSON_GetObjectItemCaseSensitive(json, "screens")) {
        screens[screen_count].name = screen->string, screens[screen_count].screen = parsed[screen_count];
        screen_count++;
    }
    main_screen.screen = find("main");
    char open[sizeof panel_name];
    memcpy(open, panel_name, sizeof open);
    if (!ui_open(open[0] != 0 ? open : NULL)) ui_open(NULL);
    return true;
}

bool ui_terminal(layout_area* out) {
    const layout_node* t = main_screen.screen != NULL ? layout_find(main_screen.screen, LAYOUT_TERMINAL) : NULL;
    if (t != NULL) *out = t->area;
    return t != NULL;
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

typedef const char* (*lookup)(const void* context, const char* name);

static const char* source_text(const void* context, const char* name) {
    (void)context;
    return source->text(source->user, name);
}

typedef struct {
    const char* list;
    size_t index;
} item_context;

static const char* item_text(const void* context, const char* name) {
    const item_context* c = context;
    return source->item(source->user, c->list, c->index, name);
}

// `text` with each {name} replaced by its value, and {name:N} padded to N cells
static size_t expand(const char* text, lookup value, const void* context, char* out) {
    size_t n = 0;
    while (*text != 0 && n < MAX_TEXT) {
        const char *end = text[0] == '{' ? strchr(text, '}') : NULL, *v = NULL;
        char name[32];
        if (end != NULL && (size_t)(end - text - 1) < sizeof name) {
            memcpy(name, text + 1, (size_t)(end - text - 1));
            name[end - text - 1] = 0;
            char* colon = strchr(name, ':');
            long pad = colon != NULL ? strtol(colon + 1, NULL, 10) : 0;
            if (colon != NULL) *colon = 0;
            size_t start = n;
            shr_text_extent e = {0};
            for (v = value(context, name); v != NULL && *v != 0 && n < MAX_TEXT; v++)
                out[n++] = *v;
            shr_pl_lyr_tilemap_measure(out + start, n - start, MAX_TEXT, 0, NULL, 0, &e, NULL);
            for (long cells = e.cols; cells < pad && n < MAX_TEXT; cells++)
                out[n++] = ' ';
            text = end + 1;
        } else {
            out[n++] = *text++;
        }
    }
    out[n] = 0;
    return n;
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

// On the middle row of `a`, cut with … to fit
static void draw_text(const layout_area* a, layout_align align, shr_text_style style, char* text, size_t length) {
    static shr_text_cluster clusters[MAX_TEXT];
    shr_text_extent e = {0};
    if (a->columns <= 0 || a->rows <= 0) return;
    shr_pl_lyr_tilemap_clear(layer, a->row, a->column, a->rows, a->columns, style);
    if (shr_pl_lyr_tilemap_measure(text, length, a->columns, 0, clusters, MAX_TEXT, &e, NULL) != SHR_OK) return;
    int32_t used = e.cols;
    if (used > a->columns) {
        size_t i = 0;
        while (i < e.clusters && clusters[i].column + clusters[i].cells <= a->columns - 1)
            i++;
        length = clusters[i].byte_offset;
        memcpy(text + length, "…", 3);
        length += 3, used = clusters[i].column + 1;
    }
    int32_t left = align == LAYOUT_LEFT ? 0 : align == LAYOUT_CENTER ? (a->columns - used) / 2 : a->columns - used;
    shr_pl_lyr_tilemap_set_text(layer, a->row + (a->rows - 1) / 2, a->column + left, text, length, style, NULL, 0, 0, NULL);
}

// A field's text: its setting's, else the value of its name
static const char* field_text(const layout_node* n, const setting* s, setting_value* v) {
    const char* text = s == NULL ? source->text(source->user, n->name) : NULL;
    if (s == NULL) return text != NULL ? text : "";
    source->get(source->user, s, v);
    return v->text;
}

// What a leaf shows; false for a list, drawn row by row
static bool content(const layout_node* n, bool focused, char* out) {
    const setting* s = n->name != NULL ? settings_find(n->name) : NULL;
    setting_value v;
    char value[80];
    if (n->kind == LAYOUT_LIST) return false;
    if (n->kind == LAYOUT_TEXT || n->kind == LAYOUT_BUTTON) {
        expand(n->text, source_text, NULL, out);
        return true;
    }
    if (n->kind == LAYOUT_FIELD) {
        const char* text = focused && editing ? edit : field_text(n, s, &v);
        if (n->secret) {
            size_t length = strnlen(text, MAX_TEXT - 1);
            memset(out, '*', length);
            out[length] = 0;
        } else {
            snprintf(out, MAX_TEXT, "%s", text);
        }
        if (focused && editing) snprintf(out + strlen(out), MAX_TEXT + 4 - strlen(out), "█");
        return true;
    }
    if (s == NULL) {
        snprintf(out, MAX_TEXT, "? %s", n->name != NULL ? n->name : "");
        return true;
    }
    source->get(source->user, s, &v);
    settings_text(s, &v, value, sizeof value);
    if (n->kind == LAYOUT_TOGGLE)
        snprintf(out, MAX_TEXT, "%s %s", v.number ? "●" : "○", value);
    else
        snprintf(out, MAX_TEXT, "◀ %s ▶", value);
    return true;
}

static void draw_list(shown_screen* s, size_t i, bool focused) {
    const layout_node* n = &s->screen->nodes[i];
    size_t count = source->count(source->user, n->name), shown_rows = n->inner.rows > 0 ? (size_t)n->inner.rows : 0;
    bool choosing = s == &panel && n->action != NULL;
    char text[MAX_TEXT + 4];
    if (choosing && chosen[i] >= count) chosen[i] = count > 0 ? count - 1 : 0;
    if (choosing && chosen[i] < first[i]) first[i] = chosen[i];
    if (choosing && shown_rows > 0 && chosen[i] >= first[i] + shown_rows) first[i] = chosen[i] - shown_rows + 1;
    size_t top = choosing ? first[i] : 0;
    uint32_t h = mix(mix(mix(2166136261u, &count, sizeof count), &top, sizeof top), (bool[]){focused}, 1);
    if (choosing) h = mix(h, &chosen[i], sizeof chosen[i]);
    for (size_t r = 0; r < shown_rows && top + r < count; r++)
        h = mix(h, text, expand(n->item, item_text, &(item_context){n->name, top + r}, text));
    if (!whole && h == s->drawn[i]) return;
    s->drawn[i] = h;
    for (size_t r = 0; r < shown_rows; r++) {
        size_t length = top + r < count ? expand(n->item, item_text, &(item_context){n->name, top + r}, text) : 0;
        bool mark = choosing && count > 0 && top + r == chosen[i];
        shr_text_style style = look(&n->style, mark && focused);
        if (mark && !focused) style.flags |= SHR_STYLE_BOLD;
        text[length] = 0;
        draw_text(&(layout_area){n->inner.column, n->inner.row + (int32_t)r, n->inner.columns, 1}, n->align, style, text, length);
    }
}

static void draw_screen(shown_screen* s) {
    char text[MAX_TEXT + 4] = "";
    for (size_t i = 0; i < s->screen->count; i++) {
        const layout_node* n = &s->screen->nodes[i];
        bool focused = s == &panel && (int)i == focus;
        if (whole && (n->style.background >= 0 || (s == &panel && i == 0))) {
            layout_style opaque = n->style;
            if (opaque.background < 0) opaque.background = 0;
            shr_pl_lyr_tilemap_clear(layer, n->area.row, n->area.column, n->area.rows, n->area.columns, look(&opaque, false));
        }
        if (whole) draw_border(n);
        if (n->area.columns <= 0 || n->area.rows <= 0) continue;
        if (n->kind == LAYOUT_LIST) draw_list(s, i, focused);
        if (n->kind < LAYOUT_TEXT || n->kind == LAYOUT_TERMINAL || !content(n, focused, text)) continue;
        bool inverse = focused || (n == pressed && over);
        size_t length = strlen(text);
        uint32_t h = mix(mix(2166136261u, text, length), &inverse, sizeof inverse);
        if (!whole && h == s->drawn[i]) continue;
        s->drawn[i] = h;
        draw_text(&n->inner, n->align, look(&n->style, inverse), text, length);
    }
}

// Each row's last cell's background on to the screen's edge
static void draw_edge(void) {
    if (edge == NULL) return;
    shr_lyr_cmd_begin(edge);
    for (int32_t r = 0; r < rows; r++) {
        const layout_node* n = panel.screen != NULL ? layout_at(panel.screen, columns - 1, r) : NULL;
        if (n == NULL && main_screen.screen != NULL) n = layout_at(main_screen.screen, columns - 1, r);
        int32_t background = n != NULL ? n->style.background : -1;
        if (n != NULL && background < 0 && panel.screen != NULL && n >= panel.screen->nodes && n < panel.screen->nodes + panel.screen->count)
            background = 0;
        if (background >= 0)
            shr_lyr_cmd_fill(edge, (shr_rect){0, r * SHR_CELL_HEIGHT, width - columns * SHR_CELL_WIDTH, (r + 1) * SHR_CELL_HEIGHT},
                             0xff000000u | (uint32_t)background);
    }
    shr_lyr_cmd_commit(edge);
}

void ui_draw(void) {
    if (whens() != laid_out) {
        lay_out();
        if (panel.screen != NULL && focus >= 0 && !visible(&panel.screen->nodes[focus])) {
            editing = edited = false;
            memset(edit, 0, sizeof edit);
            move_focus(1);
        }
    }
    if (whole) shr_pl_lyr_tilemap_clear(layer, 0, 0, rows, columns, (shr_text_style){0});
    if (main_screen.screen != NULL) draw_screen(&main_screen);
    if (panel.screen != NULL) draw_screen(&panel);
    if (whole) draw_edge();
    whole = false;
}

static ui_event set(const setting* s, setting_value* v) {
    return (ui_event){.kind = UI_SET, .name = s->name, .setting = s, .value = *v};
}

// What was typed into the field being edited as its value: by Enter, else only when anything was typed; a secret only then
static ui_event commit(bool always) {
    const layout_node* n = &panel.screen->nodes[focus];
    ui_event e = {.kind = always || edited ? UI_SET : UI_NONE, .name = n->name, .setting = settings_find(n->name)};
    if (e.kind == UI_SET) snprintf(e.value.text, sizeof e.value.text, "%s", edit);
    memset(edit, 0, sizeof edit);
    editing = edited = false;
    return e;
}

// A widget used: by a touch at `column`, or by Enter with column -1
static ui_event activate(const layout_node* n, int32_t column) {
    const setting* s = n->name != NULL ? settings_find(n->name) : NULL;
    setting_value v;
    if (n->kind == LAYOUT_BUTTON) {
        if (strcmp(n->action, "close") == 0 || (panel.screen != NULL && strcmp(n->action, panel_name) == 0))
            ui_open(NULL);
        else if (!ui_open(n->action))
            return (ui_event){.kind = UI_ACTION, .name = n->action};
        return (ui_event){.kind = UI_NONE};
    }
    size_t i = panel.screen != NULL ? (size_t)(n - panel.screen->nodes) : 0;
    if (n->kind == LAYOUT_LIST) {
        if (n->action == NULL || source->count(source->user, n->name) == 0) return (ui_event){.kind = UI_NONE};
        return (ui_event){.kind = UI_ACTION, .name = n->action, .index = chosen[i]};
    }
    // A secret is typed again, not changed
    if (n->kind == LAYOUT_FIELD && !editing) snprintf(edit, sizeof edit, "%s", n->secret ? "" : field_text(n, s, &v));
    if (n->kind == LAYOUT_FIELD) editing = true;
    if (s == NULL || n->kind == LAYOUT_FIELD) return (ui_event){.kind = UI_NONE};
    source->get(source->user, s, &v);
    settings_step(s, &v, column >= 0 && column < n->inner.column + n->inner.columns / 2 ? -1 : 1);
    return set(s, &v);
}

static bool in_panel(const layout_node* n) {
    return n != NULL && panel.screen != NULL && n >= panel.screen->nodes && n < panel.screen->nodes + panel.screen->count;
}

ui_event ui_touch(ui_touch_phase phase, int32_t column, int32_t row) {
    const layout_node* n = panel.screen != NULL ? layout_at(panel.screen, column, row) : NULL;
    ui_event e = {.kind = UI_NONE};
    if (n == NULL && main_screen.screen != NULL) n = layout_at(main_screen.screen, column, row);
    if (n != NULL && !(n->kind == LAYOUT_BUTTON || (in_panel(n) && focusable(n)))) n = NULL;
    if (phase == UI_TOUCH_DOWN) {
        if (editing && n != &panel.screen->nodes[focus]) e = commit(false);
        pressed = n;
        if (in_panel(n)) {
            focus = (int)(n - panel.screen->nodes);
            size_t at = first[focus] + (size_t)(row - n->inner.row);
            if (n->kind == LAYOUT_LIST && row >= n->inner.row && at < source->count(source->user, n->name)) chosen[focus] = at;
        }
    }
    over = pressed != NULL && n == pressed;
    if (phase != UI_TOUCH_UP) return e;
    if (over) e = activate(pressed, column);
    pressed = NULL, over = false;
    return e;
}

ui_event ui_key(ui_key_code key, const char* text) {
    ui_event e = {.kind = UI_NONE};
    if (panel.screen == NULL || focus < 0) {
        if (key == UI_KEY_ESCAPE) ui_open(NULL);
        return e;
    }
    const layout_node* n = &panel.screen->nodes[focus];
    size_t length = strlen(edit);
    if (editing) {
        const setting* s = settings_find(n->name);
        size_t limit = s != NULL && s->size < sizeof edit ? s->size : sizeof edit;
        if (key == UI_KEY_TEXT && length + strlen(text) < limit) {
            memcpy(edit + length, text, strlen(text) + 1);
            edited = true;
        } else if (key == UI_KEY_BACKSPACE && length > 0) {
            while (length > 1 && ((uint8_t)edit[length - 1] & 0xc0) == 0x80)
                length--;
            edit[length - 1] = 0;
            edited = true;
        } else if (key == UI_KEY_ENTER) {
            e = commit(!n->secret);
        } else if (key == UI_KEY_ESCAPE) {
            memset(edit, 0, sizeof edit);
            editing = edited = false;
        } else if (key == UI_KEY_TAB || key == UI_KEY_BACKTAB || key == UI_KEY_UP || key == UI_KEY_DOWN) {
            e = commit(false);
            move_focus(key == UI_KEY_TAB || key == UI_KEY_DOWN ? 1 : -1);
        }
        return e;
    }
    size_t count = n->kind == LAYOUT_LIST ? source->count(source->user, n->name) : 0;
    if (key == UI_KEY_ESCAPE) {
        ui_open(NULL);
    } else if (key == UI_KEY_TAB || key == UI_KEY_BACKTAB) {
        move_focus(key == UI_KEY_TAB ? 1 : -1);
    } else if ((key == UI_KEY_UP || key == UI_KEY_DOWN) && n->kind == LAYOUT_LIST && count > 0 &&
               (key == UI_KEY_UP ? chosen[focus] > 0 : chosen[focus] + 1 < count)) {
        chosen[focus] += key == UI_KEY_UP ? (size_t)-1 : 1;
    } else if (key == UI_KEY_UP || key == UI_KEY_DOWN) {
        move_focus(key == UI_KEY_UP ? -1 : 1);
    } else if ((key == UI_KEY_LEFT || key == UI_KEY_RIGHT) && n->kind >= LAYOUT_TOGGLE && n->kind <= LAYOUT_CHOICE) {
        e = activate(n, key == UI_KEY_LEFT ? n->inner.column : n->inner.column + n->inner.columns);
    } else if (key == UI_KEY_TEXT && n->kind == LAYOUT_FIELD) {
        activate(n, -1);
        e = ui_key(key, text);
    } else if (key == UI_KEY_ENTER) {
        e = activate(n, -1);
    }
    return e;
}
