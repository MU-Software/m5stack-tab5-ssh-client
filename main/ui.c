// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_TEXT = 256, MAX_SCREENS = 8, DIALOG_COLUMNS = 64, DIALOG_LINES = 3 };

typedef struct {
    layout* screen;
    uint32_t drawn[LAYOUT_MAX_NODES]; // hashes of what each node shows
    // The node keys go to, a field being edited and whether anything was typed into it
    int focus;
    bool editing, edited;
    char edit[128];
    // For each list: the item chosen, the first shown, and the button chosen on the chosen item (-1 for none)
    size_t chosen[LAYOUT_MAX_NODES], first[LAYOUT_MAX_NODES];
    int chosen_button[LAYOUT_MAX_NODES];
} shown_screen;

// Every dialog, built in as they are opened by function; its styles come from the layout
static const char box[] =
    "{\"style\": \"dialog\", \"border\": true, \"rows\": ["
    "{\"height\": 1, \"when\": \"dialog.title\", \"style\": \"title\", \"text\": \" {dialog.title}\"},"
    "{\"height\": 1, \"text\": \" {dialog.line.1}\"},"
    "{\"height\": 1, \"when\": \"dialog.line.2\", \"text\": \" {dialog.line.2}\"},"
    "{\"height\": 1, \"when\": \"dialog.line.3\", \"text\": \" {dialog.line.3}\"},"
    "{\"height\": 1, \"text\": \"\"},"
    "{\"height\": 1, \"when\": \"dialog.prompt\", \"columns\": [{\"width\": 1, \"text\": \"\"},"
    " {\"style\": \"field\", \"field\": \"dialog.answer\"}, {\"width\": 1, \"text\": \"\"}]},"
    "{\"height\": 1, \"when\": \"dialog.prompt\", \"text\": \"\"},"
    "{\"height\": 16, \"when\": \"dialog.list\", \"list\": \"dialog.list\", \"action\": \"dialog pick\"},"
    "{\"height\": 1, \"when\": \"dialog.list\", \"text\": \"\"},"
    "{\"height\": 1, \"columns\": [{\"text\": \"\"},"
    " {\"width\": 10, \"when\": \"dialog.cancel\", \"style\": \"button\", \"button\": \"dialog cancel\", \"text\": \"{dialog.no}\"},"
    " {\"width\": 2, \"when\": \"dialog.cancel\", \"text\": \"\"},"
    " {\"width\": 10, \"style\": \"button\", \"button\": \"dialog ok\", \"text\": \"{dialog.yes}\"}, {\"width\": 1, \"text\": \"\"}]}"
    "]}";

static shr_lyr *layer, *edge;
static int32_t columns, rows, width;
static const ui_source* source;
static cJSON *json, *box_json;
static struct {
    const char* name;
    layout* screen;
} screens[MAX_SCREENS];
static size_t screen_count;
static layout* box_layout;
// The panel over the terminal and the dialog over both
static shown_screen main_screen, panel, dialog;
static char panel_name[32];
static ui_dialog_kind dialog_kind;
static unsigned dialog_serial;
static char dialog_title[96], dialog_lines[DIALOG_LINES][160], dialog_yes[32];
// A list dialog's list, its items and the value over it; a screen dialog's screen
static char dialog_list[32], dialog_item[96], dialog_note[32], dialog_screen[32];
static bool whole;
static uint32_t laid_out; // hash of the "when"s
// The node a touch went down on, its list's item for a list's button, and whether it is still over it
static const layout_node* pressed;
static size_t pressed_item;
static bool over;

shr_status ui_start(shr_context* context, shr_pl_res_bitmap_font* font, int32_t z, int32_t columns_count, int32_t rows_count, int32_t width_pixels,
                    const ui_source* values) {
    columns = columns_count, rows = rows_count, width = width_pixels, source = values;
    shr_status status = shr_lyr_create(context, z, (shr_rect){0, 0, columns * SHR_CELL_WIDTH, rows * SHR_CELL_HEIGHT}, &layer);
    if (status == SHR_OK) status = shr_pl_lyr_tilemap_resize(layer, font, rows, columns, NULL);
    if (status == SHR_OK && width > columns * SHR_CELL_WIDTH)
        status = shr_lyr_create(context, z, (shr_rect){columns * SHR_CELL_WIDTH, 0, width, rows * SHR_CELL_HEIGHT}, &edge);
    return status;
}

// Keys and touches go to the dialog while one is open
static shown_screen* active(void) {
    return dialog.screen != NULL ? &dialog : &panel;
}

// {dialog.*} are the dialog's, the rest the source's
static const char* value_of(const char* name) {
    int line;
    if (name == NULL) return NULL;
    if (strncmp(name, "dialog.", 7) != 0) return source->text(source->user, name);
    name += 7;
    if (strcmp(name, "title") == 0) return dialog_title;
    if (strcmp(name, "line.1") == 0 && dialog_kind == UI_LIST) return source->text(source->user, dialog_note);
    if (sscanf(name, "line.%d", &line) == 1 && line >= 1 && line <= DIALOG_LINES) return dialog_lines[line - 1];
    if (strcmp(name, "yes") == 0) return dialog_yes;
    if (strcmp(name, "no") == 0) return dialog_kind == UI_LIST ? "닫기" : "취소";
    if (strcmp(name, "list") == 0) return dialog_kind == UI_LIST ? "1" : NULL;
    if (strcmp(name, "prompt") == 0) return dialog_kind >= UI_PROMPT ? "1" : NULL;
    if (strcmp(name, "cancel") == 0) return dialog_kind != UI_ALERT ? "1" : NULL;
    return NULL;
}

static bool shown(void* user, const char* name) {
    (void)user;
    const setting* s = settings_find(name);
    setting_value v;
    if (s == NULL) {
        const char* text = value_of(name);
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
    const layout* all[] = {main_screen.screen, panel.screen, dialog.screen};
    for (size_t k = 0; k < sizeof all / sizeof all[0]; k++)
        for (size_t i = 0; all[k] != NULL && i < all[k]->count; i++)
            if (all[k]->nodes[i].when != NULL && !all[k]->nodes[i].per_item) h = mix(h, (uint8_t[]){shown(NULL, all[k]->nodes[i].when)}, 1);
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
    // In the middle of the screen, as high as its border and the rows shown; a screen nearly as wide as the screen
    if (dialog.screen != NULL) {
        const layout_node* root = &dialog.screen->nodes[0];
        int32_t height = root->border ? 2 : 0, across = dialog_kind == UI_SCREEN ? columns - 4 : DIALOG_COLUMNS < columns ? DIALOG_COLUMNS : columns;
        for (int i = 0; i < root->children; i++) {
            const layout_node* c = &dialog.screen->nodes[root->first + i];
            if (c->when == NULL || shown(NULL, c->when)) height += c->unit == LAYOUT_CELLS ? c->size : 1;
        }
        layout_place(dialog.screen, (layout_area){(columns - across) / 2, (rows - height) / 2, across, height}, shown, NULL);
    }
    laid_out = whens(), whole = true;
}

static layout* find(const char* name) {
    for (size_t i = 0; i < screen_count; i++)
        if (strcmp(screens[i].name, name) == 0) return screens[i].screen;
    return NULL;
}

// Lists without an action or buttons only show
static bool focusable(const layout_node* n) {
    return n->kind > LAYOUT_LIST || n->kind == LAYOUT_BUTTON || (n->kind == LAYOUT_LIST && (n->action != NULL || n->children > 0));
}

static bool visible(const layout_node* n) {
    return n->area.columns > 0 && n->area.rows > 0;
}

static void move_focus(shown_screen* s, int direction) {
    for (size_t step = 0; s->screen != NULL && step < s->screen->count; step++) {
        s->focus = (s->focus + direction + (int)s->screen->count) % (int)s->screen->count;
        const layout_node* n = &s->screen->nodes[s->focus];
        if (focusable(n) && visible(n)) return;
    }
}

static void stop_editing(shown_screen* s) {
    memset(s->edit, 0, sizeof s->edit);
    s->editing = s->edited = false;
}

static bool in(const shown_screen* s, const layout_node* n) {
    return n != NULL && s->screen != NULL && n >= s->screen->nodes && n < s->screen->nodes + s->screen->count;
}

// Each list from its first item
static void choose_first(shown_screen* s) {
    for (size_t i = 0; s->screen != NULL && i < s->screen->count; i++)
        s->chosen[i] = s->first[i] = 0, s->chosen_button[i] = -1;
}

bool ui_open(const char* name) {
    layout* screen = name != NULL ? find(name) : NULL;
    if (name != NULL && screen == NULL) return false;
    panel.screen = screen, panel.focus = -1;
    if (!in(&dialog, pressed)) pressed = NULL;
    stop_editing(&panel);
    snprintf(panel_name, sizeof panel_name, "%s", name != NULL ? name : "");
    choose_first(&panel);
    lay_out();
    bool leading = false;
    for (size_t i = 0; screen != NULL && i < screen->count; i++) {
        const layout_node* n = &screen->nodes[i];
        // A list with an action or an editor first, else a button or a list
        bool leads = n->kind != LAYOUT_BUTTON && (n->kind != LAYOUT_LIST || n->action != NULL);
        if (focusable(n) && visible(n) && (panel.focus < 0 || (!leading && leads))) panel.focus = (int)i, leading = leads;
    }
    return true;
}

const char* ui_panel(void) {
    return panel.screen != NULL ? panel_name : NULL;
}

// The box's field shows * for a password, and its list is the list dialog's
static void fit_box(void) {
    for (size_t i = 0; i < box_layout->count; i++) {
        layout_node* n = &box_layout->nodes[i];
        if (n->kind == LAYOUT_FIELD) n->secret = dialog_kind == UI_PASSWORD;
        if (n->kind == LAYOUT_LIST) n->name = dialog_list, n->item = dialog_item;
    }
}

// `screen` as the dialog of `kind`: its field or list first, else the button that does nothing
static unsigned show_dialog(layout* screen, ui_dialog_kind kind) {
    if (screen == NULL) return 0;
    dialog_kind = kind;
    dialog.screen = screen, dialog.focus = -1, pressed = NULL;
    stop_editing(&dialog);
    if (box_layout != NULL) fit_box();
    choose_first(&dialog);
    lay_out();
    const char* idle = kind == UI_ALERT ? "dialog ok" : "dialog cancel";
    layout_kind first = kind >= UI_PROMPT ? LAYOUT_FIELD : kind == UI_LIST ? LAYOUT_LIST : LAYOUT_BUTTON;
    for (size_t i = 0; dialog.focus < 0 && i < screen->count; i++) {
        const layout_node* n = &screen->nodes[i];
        if (n->kind == first && (first != LAYOUT_BUTTON || strcmp(n->action, idle) == 0)) dialog.focus = (int)i;
    }
    dialog.editing = kind >= UI_PROMPT;
    return ++dialog_serial;
}

unsigned ui_dialog(ui_dialog_kind kind, const char* title, const char* text, const char* yes) {
    snprintf(dialog_title, sizeof dialog_title, "%s", title != NULL ? title : "");
    snprintf(dialog_yes, sizeof dialog_yes, "%s", yes);
    for (size_t i = 0; i < DIALOG_LINES; i++) {
        const char* end = strchr(text, '\n');
        size_t length = end != NULL ? (size_t)(end - text) : strlen(text);
        snprintf(dialog_lines[i], sizeof dialog_lines[i], "%.*s", (int)length, text);
        text += end != NULL ? length + 1 : length;
    }
    return show_dialog(box_layout, kind);
}

unsigned ui_dialog_screen(const char* name) {
    snprintf(dialog_screen, sizeof dialog_screen, "%s", name);
    return show_dialog(find(name), UI_SCREEN);
}

unsigned ui_dialog_list(const char* title, const char* list, const char* item, const char* note, const char* yes) {
    snprintf(dialog_list, sizeof dialog_list, "%s", list);
    snprintf(dialog_item, sizeof dialog_item, "%s", item);
    snprintf(dialog_note, sizeof dialog_note, "%s", note);
    return ui_dialog(UI_LIST, title, "", yes);
}

size_t ui_dialog_chosen(void) {
    for (size_t i = 0; dialog.screen != NULL && i < dialog.screen->count; i++)
        if (dialog.screen->nodes[i].kind == LAYOUT_LIST) return dialog.chosen[i];
    return 0;
}

void ui_dialog_close(void) {
    if (dialog.screen == NULL) return;
    dialog.screen = NULL, pressed = NULL;
    stop_editing(&dialog);
    lay_out();
}

unsigned ui_dialog_shown(void) {
    return dialog.screen != NULL ? dialog_serial : 0;
}

void ui_dialog_answer(char* out, size_t size) {
    snprintf(out, size, "%s", dialog.edit);
    stop_editing(&dialog);
}

bool ui_load(const char* text, char* error, size_t size) {
    cJSON* next = cJSON_Parse(text);
    const cJSON* screen;
    size_t count = 0;
    layout *parsed[MAX_SCREENS], *box_parsed = NULL;
    if (next == NULL) {
        snprintf(error, size, "not JSON, near byte %zu", (size_t)(cJSON_GetErrorPtr() - text));
        return false;
    }
    if (box_json == NULL) box_json = cJSON_Parse(box);
    bool ok = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(next, "screens"), "main") != NULL;
    if (!ok) snprintf(error, size, "no screen main");
    cJSON_ArrayForEach(screen, cJSON_GetObjectItemCaseSensitive(next, "screens")) {
        if (!ok) break;
        if (count == MAX_SCREENS) {
            snprintf(error, size, "more than %d screens", MAX_SCREENS);
            ok = false;
        } else if ((parsed[count] = layout_parse(next, screen, error, size)) == NULL) {
            ok = false;
        } else {
            count++;
        }
    }
    if (ok && (box_parsed = layout_parse(next, box_json, error, size)) == NULL) ok = false;
    if (!ok) {
        while (count > 0)
            free(parsed[--count]);
        cJSON_Delete(next);
        return false;
    }
    for (size_t i = 0; i < screen_count; i++)
        free(screens[i].screen);
    free(box_layout);
    cJSON_Delete(json);
    json = next, screen_count = 0, box_layout = box_parsed;
    cJSON_ArrayForEach(screen, cJSON_GetObjectItemCaseSensitive(json, "screens")) {
        screens[screen_count].name = screen->string, screens[screen_count].screen = parsed[screen_count];
        screen_count++;
    }
    main_screen.screen = find("main");
    if (box_layout != NULL) fit_box();
    // A screen's nodes may have moved
    if (dialog.screen != NULL && dialog_kind == UI_SCREEN) dialog.screen = find(dialog_screen), dialog.focus = -1, move_focus(&dialog, 1);
    if (dialog.screen != NULL && dialog_kind != UI_SCREEN) dialog.screen = box_layout;
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
    return value_of(name);
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
    const char* text = s == NULL ? value_of(n->name) : NULL;
    if (s == NULL) return text != NULL ? text : "";
    source->get(source->user, s, v);
    return v->text;
}

// What a leaf shows; false for a list, drawn row by row
static bool content(const shown_screen* screen, const layout_node* n, bool focused, char* out) {
    const setting* s = n->name != NULL ? settings_find(n->name) : NULL;
    setting_value v;
    char value[80];
    if (n->kind == LAYOUT_LIST) return false;
    if (n->kind == LAYOUT_TEXT || n->kind == LAYOUT_BUTTON) {
        expand(n->text, source_text, NULL, out);
        return true;
    }
    if (n->kind == LAYOUT_FIELD) {
        bool editing = focused && screen->editing;
        const char* text = editing || screen == &dialog ? screen->edit : field_text(n, s, &v);
        if (n->secret) {
            size_t length = strnlen(text, MAX_TEXT - 1);
            memset(out, '*', length);
            out[length] = 0;
        } else {
            snprintf(out, MAX_TEXT, "%s", text);
        }
        if (editing) snprintf(out + strlen(out), MAX_TEXT + 4 - strlen(out), "█");
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

// Whether button b of `list` is on item `index`: its "when" names one of the item's fields
static bool on_item(const layout_node* b, const layout_node* list, size_t index) {
    const char* v = b->when != NULL ? source->item(source->user, list->name, index, b->when) : NULL;
    return b->when == NULL || (v != NULL && v[0] != 0);
}

// Where button k of list n starts on each row: the buttons end the row, each followed by a cell
static int32_t button_column(const layout* screen, const layout_node* n, int k) {
    int32_t at = n->inner.column + n->inner.columns;
    for (int j = n->children - 1; j >= k; j--)
        at -= screen->nodes[n->first + j].size + 1;
    return at;
}

// The first of list n's buttons on item `index`, -1 for none
static int first_button(const layout* screen, const layout_node* n, size_t index) {
    for (int k = 0; k < n->children; k++)
        if (on_item(&screen->nodes[n->first + k], n, index)) return k;
    return -1;
}

// Item `index` of list n as its row shows it: its text, returning its length, and each button's label, empty where it is not
static size_t list_row(const layout* screen, const layout_node* n, size_t index, char* text, char labels[][MAX_TEXT + 4], int buttons) {
    const item_context c = {n->name, index};
    for (int k = 0; k < buttons; k++) {
        const layout_node* b = &screen->nodes[n->first + k];
        labels[k][on_item(b, n, index) ? expand(b->text, item_text, &c, labels[k]) : 0] = 0;
    }
    return expand(n->item, item_text, &c, text);
}

// Whether anything was drawn
static bool draw_list(shown_screen* s, size_t i, bool focused) {
    const layout_node* n = &s->screen->nodes[i];
    size_t count = source->count(source->user, n->name), shown_rows = n->inner.rows > 0 ? (size_t)n->inner.rows : 0;
    int buttons = n->children;
    bool choosing = s != &main_screen && focusable(n);
    char text[MAX_TEXT + 4], labels[LAYOUT_MAX_BUTTONS][MAX_TEXT + 4];
    if (choosing && s->chosen[i] >= count) s->chosen[i] = count > 0 ? count - 1 : 0;
    if (choosing && s->chosen[i] < s->first[i]) s->first[i] = s->chosen[i];
    if (choosing && shown_rows > 0 && s->chosen[i] >= s->first[i] + shown_rows) s->first[i] = s->chosen[i] - shown_rows + 1;
    // A button the chosen item no longer has; a list whose rows do nothing has one chosen while it can
    if (s->chosen_button[i] >= 0 && (count == 0 || !on_item(&s->screen->nodes[n->first + s->chosen_button[i]], n, s->chosen[i])))
        s->chosen_button[i] = -1;
    if (n->action == NULL && s->chosen_button[i] < 0 && count > 0) s->chosen_button[i] = first_button(s->screen, n, s->chosen[i]);
    size_t top = choosing ? s->first[i] : 0;
    bool pressing = pressed != NULL && over && pressed >= &s->screen->nodes[n->first] && pressed < &s->screen->nodes[n->first + buttons];
    uint32_t h = mix(mix(mix(2166136261u, &count, sizeof count), &top, sizeof top), (bool[]){focused, pressing}, 2);
    if (choosing) h = mix(mix(h, &s->chosen[i], sizeof s->chosen[i]), &s->chosen_button[i], sizeof s->chosen_button[i]);
    size_t button = pressing ? (size_t)(pressed - s->screen->nodes) : 0;
    if (pressing) h = mix(mix(h, &button, sizeof button), &pressed_item, sizeof pressed_item);
    for (size_t r = 0; r < shown_rows && top + r < count; r++) {
        h = mix(h, text, list_row(s->screen, n, top + r, text, labels, buttons));
        for (int k = 0; k < buttons; k++)
            h = mix(h, labels[k], strlen(labels[k]) + 1);
    }
    if (!whole && h == s->drawn[i]) return false;
    s->drawn[i] = h;
    int32_t text_columns = buttons > 0 ? button_column(s->screen, n, 0) - n->inner.column : n->inner.columns;
    for (size_t r = 0; r < shown_rows; r++) {
        // Rows that do nothing show no choice, only their chosen button
        bool item = top + r < count, mark = choosing && item && top + r == s->chosen[i], picks = mark && n->action != NULL;
        bool marked = picks && focused && s->chosen_button[i] < 0;
        size_t length = item ? list_row(s->screen, n, top + r, text, labels, buttons) : 0;
        int32_t at = n->inner.row + (int32_t)r;
        layout_style row = n->style;
        if (item && (top + r) % 2 == 0 && n->stripe >= 0) row.background = n->stripe;
        shr_pl_lyr_tilemap_clear(layer, at, n->inner.column, 1, n->inner.columns, look(&row, false));
        shr_text_style style = look(&row, marked);
        if (picks && !marked) style.flags |= SHR_STYLE_BOLD;
        text[length] = 0;
        draw_text(&(layout_area){n->inner.column, at, text_columns, 1}, n->align, style, text, length);
        for (int k = 0; k < buttons; k++) {
            const layout_node* b = &s->screen->nodes[n->first + k];
            bool on = item && labels[k][0] != 0;
            bool inverse = (on && mark && focused && s->chosen_button[i] == k) || (pressing && pressed == b && pressed_item == top + r);
            if (!on) labels[k][0] = 0;
            draw_text(&(layout_area){button_column(s->screen, n, k), at, b->size, 1}, LAYOUT_CENTER, look(on ? &b->style : &row, inverse), labels[k],
                      strlen(labels[k]));
        }
    }
    return true;
}

// Whether anything was drawn
static bool draw_screen(shown_screen* s) {
    char text[MAX_TEXT + 4] = "";
    bool drew = whole;
    for (size_t i = 0; i < s->screen->count; i++) {
        const layout_node* n = &s->screen->nodes[i];
        bool focused = s == active() && (int)i == s->focus;
        if (whole && (n->style.background >= 0 || (s != &main_screen && i == 0))) {
            layout_style opaque = n->style;
            if (opaque.background < 0) opaque.background = 0;
            shr_pl_lyr_tilemap_clear(layer, n->area.row, n->area.column, n->area.rows, n->area.columns, look(&opaque, false));
        }
        if (whole) draw_border(n);
        if (n->area.columns <= 0 || n->area.rows <= 0) continue;
        if (n->kind == LAYOUT_LIST) drew |= draw_list(s, i, focused);
        if (n->kind < LAYOUT_TEXT || n->kind == LAYOUT_TERMINAL || !content(s, n, focused, text)) continue;
        bool inverse = focused || (n == pressed && over);
        size_t length = strlen(text);
        uint32_t h = mix(mix(2166136261u, text, length), &inverse, sizeof inverse);
        if (!whole && h == s->drawn[i]) continue;
        s->drawn[i] = h, drew = true;
        draw_text(&n->inner, n->align, look(&n->style, inverse), text, length);
    }
    return drew;
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
    shown_screen* each[] = {&panel, &dialog};
    if (whens() != laid_out) {
        lay_out();
        for (size_t k = 0; k < 2; k++)
            if (each[k]->screen != NULL && each[k]->focus >= 0 && !visible(&each[k]->screen->nodes[each[k]->focus])) {
                stop_editing(each[k]);
                move_focus(each[k], 1);
            }
    }
    if (whole) shr_pl_lyr_tilemap_clear(layer, 0, 0, rows, columns, (shr_text_style){0});
    bool under = false, all = whole;
    if (main_screen.screen != NULL) under |= draw_screen(&main_screen);
    if (panel.screen != NULL) under |= draw_screen(&panel);
    // Whatever was drawn under it may have covered it
    whole = under;
    if (dialog.screen != NULL) draw_screen(&dialog);
    if (all) draw_edge();
    whole = false;
}

static ui_event set(const setting* s, setting_value* v) {
    return (ui_event){.kind = UI_SET, .name = s->name, .setting = s, .value = *v};
}

// What was typed into the panel's field being edited as its value: by Enter, else only when anything was typed; a secret
// only then. A dialog keeps what was typed for its answer.
static ui_event commit(bool always) {
    shown_screen* s = active();
    const layout_node* n = &s->screen->nodes[s->focus];
    ui_event e = {.kind = s == &panel && (always || s->edited) ? UI_SET : UI_NONE, .name = n->name, .setting = settings_find(n->name)};
    if (e.kind == UI_SET) snprintf(e.value.text, sizeof e.value.text, "%.*s", (int)sizeof e.value.text - 1, s->edit); // GCC truncation
    if (s == &dialog)
        s->editing = false;
    else
        stop_editing(s);
    return e;
}

// A dialog's button pressed
static ui_event dialog_action(const char* action) {
    return (ui_event){.kind = UI_ACTION, .name = action, .index = dialog_serial};
}

// A widget used: by a touch at `column`, or by Enter with column -1
static ui_event activate(const layout_node* n, int32_t column) {
    shown_screen* screen = active();
    const setting* s = n->name != NULL ? settings_find(n->name) : NULL;
    setting_value v;
    if (n->per_item) return (ui_event){.kind = UI_ACTION, .name = n->action, .index = screen->chosen[screen->focus]};
    if (n->kind == LAYOUT_BUTTON) {
        if (screen == &dialog) return dialog_action(n->action);
        // The open panel's button does nothing
        if (strcmp(n->action, panel_name) != 0 && !ui_open(n->action)) return (ui_event){.kind = UI_ACTION, .name = n->action};
        return (ui_event){.kind = UI_NONE};
    }
    if (n->kind == LAYOUT_LIST) {
        if (n->action == NULL || source->count(source->user, n->name) == 0) return (ui_event){.kind = UI_NONE};
        if (screen == &dialog) return dialog_action(n->action);
        return (ui_event){.kind = UI_ACTION, .name = n->action, .index = screen->chosen[n - screen->screen->nodes]};
    }
    // A secret is typed again, not changed; a dialog's answer stays
    if (n->kind == LAYOUT_FIELD && !screen->editing && screen == &panel)
        snprintf(screen->edit, sizeof screen->edit, "%s", n->secret ? "" : field_text(n, s, &v));
    if (n->kind == LAYOUT_FIELD) screen->editing = true;
    if (s == NULL || n->kind == LAYOUT_FIELD) return (ui_event){.kind = UI_NONE};
    source->get(source->user, s, &v);
    settings_step(s, &v, column >= 0 && column < n->inner.column + n->inner.columns / 2 ? -1 : 1);
    return set(s, &v);
}

// The button of list n under `column` on item `index`'s row, -1 for none
static int button_under(const layout* screen, const layout_node* n, int32_t column, size_t index) {
    for (int k = 0; k < n->children; k++) {
        const layout_node* b = &screen->nodes[n->first + k];
        int32_t at = button_column(screen, n, k);
        if (column >= at && column < at + b->size && on_item(b, n, index)) return k;
    }
    return -1;
}

ui_event ui_touch(ui_touch_phase phase, int32_t column, int32_t row) {
    shown_screen* s = active();
    const layout_node *n = s->screen != NULL ? layout_at(s->screen, column, row) : NULL, *list = NULL;
    ui_event e = {.kind = UI_NONE};
    size_t index = 0;
    int k = -1;
    // Over a dialog nothing else takes touches
    if (n == NULL && s == &panel && main_screen.screen != NULL) n = layout_at(main_screen.screen, column, row);
    if (n != NULL && !(n->kind == LAYOUT_BUTTON || (in(s, n) && focusable(n)))) n = NULL;
    if (n != NULL && n->kind == LAYOUT_LIST) {
        list = n, index = s->first[n - s->screen->nodes] + (size_t)(row - n->inner.row);
        // Rows past a list's last item are no item
        if (row < n->inner.row || index >= source->count(source->user, n->name)) n = list = NULL;
    }
    if (list != NULL && (k = button_under(s->screen, list, column, index)) >= 0) n = &s->screen->nodes[list->first + k];
    // A row that does nothing is no button either
    if (list != NULL && k < 0 && list->action == NULL) n = list = NULL;
    if (phase == UI_TOUCH_DOWN) {
        // Outside a dialog a touch does nothing
        if (s->editing && n != &s->screen->nodes[s->focus] && (s == &panel || n != NULL)) e = commit(false);
        pressed = n, pressed_item = index;
        if (in(s, list != NULL ? list : n)) s->focus = (int)((list != NULL ? list : n) - s->screen->nodes);
        if (list != NULL) s->chosen[s->focus] = index, s->chosen_button[s->focus] = k;
    }
    over = pressed != NULL && n == pressed && index == pressed_item;
    if (phase != UI_TOUCH_UP) return e;
    if (over) e = activate(pressed, column);
    pressed = NULL, over = false;
    return e;
}

ui_event ui_key(ui_key_code key, const char* text) {
    shown_screen* screen = active();
    ui_event e = {.kind = UI_NONE};
    bool in_dialog = screen == &dialog;
    if (in_dialog && key == UI_KEY_ESCAPE) return dialog_action("dialog cancel");
    if (screen->screen == NULL || screen->focus < 0) return e;
    const layout_node* n = &screen->screen->nodes[screen->focus];
    size_t length = strlen(screen->edit);
    if (screen->editing) {
        // A panel's field becomes a setting's value or a ui_event's text
        const setting* s = settings_find(n->name);
        size_t limit = in_dialog ? sizeof screen->edit : s != NULL ? s->size : sizeof e.value.text;
        if (key == UI_KEY_TEXT && length + strlen(text) < limit) {
            memcpy(screen->edit + length, text, strlen(text) + 1);
            screen->edited = true;
        } else if (key == UI_KEY_BACKSPACE && length > 0) {
            while (length > 1 && ((uint8_t)screen->edit[length - 1] & 0xc0) == 0x80)
                length--;
            screen->edit[length - 1] = 0;
            screen->edited = true;
        } else if (key == UI_KEY_ENTER) {
            e = in_dialog ? dialog_action("dialog ok") : commit(!n->secret);
        } else if (key == UI_KEY_ESCAPE) {
            stop_editing(screen);
        } else if (key == UI_KEY_TAB || key == UI_KEY_BACKTAB || key == UI_KEY_UP || key == UI_KEY_DOWN) {
            e = commit(false);
            move_focus(screen, key == UI_KEY_TAB || key == UI_KEY_DOWN ? 1 : -1);
        }
        return e;
    }
    size_t count = n->kind == LAYOUT_LIST ? source->count(source->user, n->name) : 0;
    if (key == UI_KEY_TAB || key == UI_KEY_BACKTAB) {
        move_focus(screen, key == UI_KEY_TAB ? 1 : -1);
    } else if ((key == UI_KEY_UP || key == UI_KEY_DOWN) && n->kind == LAYOUT_LIST && count > 0 &&
               (key == UI_KEY_UP ? screen->chosen[screen->focus] > 0 : screen->chosen[screen->focus] + 1 < count)) {
        screen->chosen[screen->focus] += key == UI_KEY_UP ? (size_t)-1 : 1;
    } else if (key == UI_KEY_UP || key == UI_KEY_DOWN || (in_dialog && (key == UI_KEY_LEFT || key == UI_KEY_RIGHT))) {
        move_focus(screen, key == UI_KEY_UP || key == UI_KEY_LEFT ? -1 : 1);
    } else if ((key == UI_KEY_LEFT || key == UI_KEY_RIGHT) && n->kind == LAYOUT_LIST && count > 0) {
        // The next of the chosen item's buttons, or the item itself if it does anything
        int *k = &screen->chosen_button[screen->focus], step = key == UI_KEY_LEFT ? n->children : 1;
        for (int tries = 0; tries <= n->children; tries++) {
            *k = (*k + 1 + step) % (n->children + 1) - 1;
            if (*k >= 0 ? on_item(&screen->screen->nodes[n->first + *k], n, screen->chosen[screen->focus]) : n->action != NULL) break;
        }
    } else if ((key == UI_KEY_LEFT || key == UI_KEY_RIGHT) && n->kind >= LAYOUT_TOGGLE && n->kind <= LAYOUT_CHOICE) {
        e = activate(n, key == UI_KEY_LEFT ? n->inner.column : n->inner.column + n->inner.columns);
    } else if (key == UI_KEY_TEXT && n->kind == LAYOUT_FIELD) {
        activate(n, -1);
        e = ui_key(key, text);
    } else if (key == UI_KEY_ENTER && n->kind == LAYOUT_LIST && screen->chosen_button[screen->focus] >= 0) {
        const layout_node* b = &screen->screen->nodes[n->first + screen->chosen_button[screen->focus]];
        if (count > 0 && on_item(b, n, screen->chosen[screen->focus])) e = activate(b, -1);
    } else if (key == UI_KEY_ENTER) {
        e = activate(n, -1);
    }
    return e;
}
