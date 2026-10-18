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

// What the screens show
typedef struct {
    // The text for {name}, NULL for none
    const char* (*text)(void* user, const char* name);
    // List `name`: its length, {field} of item `index`, and the item chosen when a panel opens
    size_t (*count)(void* user, const char* name);
    const char* (*item)(void* user, const char* name, size_t index, const char* field);
    size_t (*chosen)(void* user, const char* name);
    void (*get)(void* user, const setting* s, setting_value* out);
    void* user;
} ui_source;

typedef enum { UI_NONE, UI_ACTION, UI_SET } ui_event_kind;

// An action with the index of its list's chosen item (a dialog's button: the dialog's serial, else 0), or a field's or
// another setting's new value: a field that is no setting has setting NULL
typedef struct {
    ui_event_kind kind;
    const char* name;
    size_t index;
    const setting* setting;
    setting_value value;
} ui_event;

// A tilemap layer at z over columns x rows cells, transparent where the screen has no background, on a screen
// `width` pixels wide (the cells' background goes on to its edge); `source` is kept
shr_status ui_start(shr_context* context, shr_pl_res_bitmap_font* font, int32_t z, int32_t columns, int32_t rows, int32_t width,
                    const ui_source* source);
// The screens of a layout file: "main", and panels over the terminal that buttons named after them open and close
// ("close" closes); on error the screens stay as they were
bool ui_load(const char* text, char* error, size_t size);
// Draws what changed; lays the screens out again when a "when" changed
void ui_draw(void);
// Opens panel `name` (NULL closes, opening the home panel if there is one); false if there is none
bool ui_open(const char* name);
// The panel that closing opens instead, NULL for none
void ui_home(const char* name);
// The open panel, NULL for none: keys then go to it
const char* ui_panel(void);
// The terminal's cells; false without one
bool ui_terminal(layout_area* out);
// A touch on cell (column, row)
ui_event ui_touch(ui_touch_phase phase, int32_t column, int32_t row);
// A key for the open dialog, else the open panel: arrows and Tab move (←→ along a list row's buttons), Enter chooses,
// Escape closes; `text` (UTF-8) for UI_KEY_TEXT
ui_event ui_key(ui_key_code key, const char* text);

typedef enum { UI_ALERT, UI_CONFIRM, UI_PROMPT, UI_PASSWORD } ui_dialog_kind;

// A box over the screens, which alone takes keys and touches until ui_dialog_close: `title` (NULL for none), up to three
// \n-separated lines of `text`, a field for a prompt (secret for a password), and button "dialog ok" labelled `yes` with
// "dialog cancel" but for an alert. Its buttons' events carry the serial it returns, 0 without a layout, as index.
unsigned ui_dialog(ui_dialog_kind kind, const char* title, const char* text, const char* yes);
void ui_dialog_close(void);
// The serial of the dialog shown, 0 for none
unsigned ui_dialog_shown(void);
// What was typed into the prompt, which is then wiped
void ui_dialog_answer(char* out, size_t size);
