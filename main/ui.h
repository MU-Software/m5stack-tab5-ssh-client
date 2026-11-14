// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <shiroko/shiroko.h>

#include "layout.h"
#include "settings.h"

typedef enum { UI_TOUCH_DOWN, UI_TOUCH_MOVE, UI_TOUCH_UP } ui_touch_phase;
typedef enum {
    UI_KEY_UP,
    UI_KEY_DOWN,
    UI_KEY_LEFT,
    UI_KEY_RIGHT,
    UI_KEY_TAB,
    UI_KEY_BACKTAB,
    UI_KEY_ENTER,
    UI_KEY_ESCAPE,
    UI_KEY_BACKSPACE,
    UI_KEY_TEXT
} ui_key_code;

typedef struct {
    const char* (*text)(void* user, const char* name);
    size_t (*count)(void* user, const char* name);
    const char* (*item)(void* user, const char* name, size_t index, const char* field);
    void (*get)(void* user, const setting* s, setting_value* out);
    void* user;
} ui_source;

typedef enum { UI_NONE, UI_ACTION, UI_SET } ui_event_kind;

// index: the list's chosen item, a dialog's serial, else 0; setting NULL: a field that is no setting
typedef struct {
    ui_event_kind kind;
    const char* name;
    size_t index;
    const setting* setting;
    setting_value value;
} ui_event;

// width: pixels, for the cells' background to reach that edge; `source` is kept
shr_status ui_start(shr_context* context, shr_pl_res_bitmap_font* font, int32_t z, int32_t columns, int32_t rows, int32_t width,
                    const ui_source* source);
// Buttons named after a panel open it; on error the screens stay as they were
bool ui_load(const char* text, char* error, size_t size);
void ui_draw(void);
// NULL closes
bool ui_open(const char* name);
const char* ui_panel(void);
bool ui_terminal(layout_area* out);
// In cells
ui_event ui_touch(ui_touch_phase phase, int32_t column, int32_t row);
// For the open dialog, else the open panel; `text`: UTF-8, for UI_KEY_TEXT
ui_event ui_key(ui_key_code key, const char* text);

typedef enum { UI_ALERT, UI_CONFIRM, UI_LIST, UI_SCREEN, UI_PROMPT, UI_PASSWORD } ui_dialog_kind;

// Takes all keys and touches until ui_dialog_close. `title` NULL for none, `text` up to three \n-separated lines. Buttons
// "dialog ok" (`yes`) and, but for an alert, "dialog cancel" carry the returned serial as index; 0 without a layout
unsigned ui_dialog(ui_dialog_kind kind, const char* title, const char* text, const char* yes);
// An item chosen is "dialog pick"; `note`: a source value shown over the list
unsigned ui_dialog_list(const char* title, const char* list, const char* item, const char* note, const char* yes);
unsigned ui_dialog_screen(const char* name);
size_t ui_dialog_chosen(void);
void ui_dialog_close(void);
// Its serial, 0 for none
unsigned ui_dialog_shown(void);
// The prompt's text, then wiped
void ui_dialog_answer(char* out, size_t size);
