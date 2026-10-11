// SPDX-License-Identifier: GPL-3.0-or-later
#include "keys.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "terminal.h"

typedef struct {
    GhosttyKey key;
    char text, shifted;
} key_info;

// US layout, beside letters, digits and F1-F12
static const key_info others[] = {
    [0x28] = {GHOSTTY_KEY_ENTER},
    [0x29] = {GHOSTTY_KEY_ESCAPE},
    [0x2a] = {GHOSTTY_KEY_BACKSPACE},
    [0x2b] = {GHOSTTY_KEY_TAB},
    [0x2c] = {GHOSTTY_KEY_SPACE, ' ', ' '},
    [0x2d] = {GHOSTTY_KEY_MINUS, '-', '_'},
    [0x2e] = {GHOSTTY_KEY_EQUAL, '=', '+'},
    [0x2f] = {GHOSTTY_KEY_BRACKET_LEFT, '[', '{'},
    [0x30] = {GHOSTTY_KEY_BRACKET_RIGHT, ']', '}'},
    [0x31] = {GHOSTTY_KEY_BACKSLASH, '\\', '|'},
    [0x33] = {GHOSTTY_KEY_SEMICOLON, ';', ':'},
    [0x34] = {GHOSTTY_KEY_QUOTE, '\'', '"'},
    [0x35] = {GHOSTTY_KEY_BACKQUOTE, '`', '~'},
    [0x36] = {GHOSTTY_KEY_COMMA, ',', '<'},
    [0x37] = {GHOSTTY_KEY_PERIOD, '.', '>'},
    [0x38] = {GHOSTTY_KEY_SLASH, '/', '?'},
    [0x39] = {GHOSTTY_KEY_CAPS_LOCK},
    [0x46] = {GHOSTTY_KEY_PRINT_SCREEN},
    [0x47] = {GHOSTTY_KEY_SCROLL_LOCK},
    [0x48] = {GHOSTTY_KEY_PAUSE},
    [0x49] = {GHOSTTY_KEY_INSERT},
    [0x4a] = {GHOSTTY_KEY_HOME},
    [0x4b] = {GHOSTTY_KEY_PAGE_UP},
    [0x4c] = {GHOSTTY_KEY_DELETE},
    [0x4d] = {GHOSTTY_KEY_END},
    [0x4e] = {GHOSTTY_KEY_PAGE_DOWN},
    [0x4f] = {GHOSTTY_KEY_ARROW_RIGHT},
    [0x50] = {GHOSTTY_KEY_ARROW_LEFT},
    [0x51] = {GHOSTTY_KEY_ARROW_DOWN},
    [0x52] = {GHOSTTY_KEY_ARROW_UP},
    [0x53] = {GHOSTTY_KEY_NUM_LOCK},
    [0x54] = {GHOSTTY_KEY_NUMPAD_DIVIDE, '/', '/'},
    [0x55] = {GHOSTTY_KEY_NUMPAD_MULTIPLY, '*', '*'},
    [0x56] = {GHOSTTY_KEY_NUMPAD_SUBTRACT, '-', '-'},
    [0x57] = {GHOSTTY_KEY_NUMPAD_ADD, '+', '+'},
    [0x58] = {GHOSTTY_KEY_NUMPAD_ENTER},
    [0x59] = {GHOSTTY_KEY_NUMPAD_1, '1', '1'},
    [0x5a] = {GHOSTTY_KEY_NUMPAD_2, '2', '2'},
    [0x5b] = {GHOSTTY_KEY_NUMPAD_3, '3', '3'},
    [0x5c] = {GHOSTTY_KEY_NUMPAD_4, '4', '4'},
    [0x5d] = {GHOSTTY_KEY_NUMPAD_5, '5', '5'},
    [0x5e] = {GHOSTTY_KEY_NUMPAD_6, '6', '6'},
    [0x5f] = {GHOSTTY_KEY_NUMPAD_7, '7', '7'},
    [0x60] = {GHOSTTY_KEY_NUMPAD_8, '8', '8'},
    [0x61] = {GHOSTTY_KEY_NUMPAD_9, '9', '9'},
    [0x62] = {GHOSTTY_KEY_NUMPAD_0, '0', '0'},
    [0x63] = {GHOSTTY_KEY_NUMPAD_DECIMAL, '.', '.'},
    [0x64] = {GHOSTTY_KEY_INTL_BACKSLASH, '\\', '|'},
    [0x65] = {GHOSTTY_KEY_CONTEXT_MENU},
};

#define REPEAT_DELAY_US    500000
#define REPEAT_INTERVAL_US 33000

static QueueHandle_t events;
static esp_timer_handle_t repeat;
static key_event repeating;

static void repeat_key(void* arg) {
    (void)arg;
    xQueueSend(events, &repeating, 0);
    esp_timer_start_once(repeat, REPEAT_INTERVAL_US);
}

esp_err_t keys_start(void) {
    events = xQueueCreate(32, sizeof(key_event));
    if (events == NULL) return ESP_ERR_NO_MEM;
    return esp_timer_create(&(esp_timer_create_args_t){.callback = repeat_key, .name = "key repeat"}, &repeat);
}

void keys_push(const key_event* event) {
    xQueueSend(events, event, 0);
    if (event->action == GHOSTTY_KEY_ACTION_PRESS || event->usage == repeating.usage) esp_timer_stop(repeat);
    if (event->action == GHOSTTY_KEY_ACTION_PRESS && event->usage != 0x39) {
        repeating = *event;
        repeating.action = GHOSTTY_KEY_ACTION_REPEAT;
        esp_timer_start_once(repeat, REPEAT_DELAY_US);
    }
}

static key_info find(uint8_t usage) {
    if (usage >= 0x04 && usage <= 0x1d)
        return (key_info){(GhosttyKey)(GHOSTTY_KEY_A + usage - 0x04), (char)('a' + usage - 0x04), (char)('A' + usage - 0x04)};
    if (usage >= 0x1e && usage <= 0x27) {
        int digit = (usage - 0x1d) % 10;
        return (key_info){(GhosttyKey)(GHOSTTY_KEY_DIGIT_0 + digit), (char)('0' + digit), ")!@#$%^&*("[digit]};
    }
    if (usage >= 0x3a && usage <= 0x45) return (key_info){.key = (GhosttyKey)(GHOSTTY_KEY_F1 + usage - 0x3a)};
    return usage < sizeof others / sizeof others[0] ? others[usage] : (key_info){.key = GHOSTTY_KEY_UNIDENTIFIED};
}

static size_t encode(const key_event* e, char* out, size_t size) {
    key_info k = find(e->usage);
    GhosttyKeyEvent event;
    if (k.key == GHOSTTY_KEY_UNIDENTIFIED || ghostty_key_event_new(&terminal_allocator, &event) != GHOSTTY_SUCCESS) return 0;
    uint8_t m = e->modifiers;
    GhosttyMods mods = (m & 0x22 ? GHOSTTY_MODS_SHIFT : 0) | (m & 0x11 ? GHOSTTY_MODS_CTRL : 0) | (m & 0x44 ? GHOSTTY_MODS_ALT : 0) |
                       (m & 0x88 ? GHOSTTY_MODS_SUPER : 0) | (e->caps_lock ? GHOSTTY_MODS_CAPS_LOCK : 0);
    bool letter = k.text >= 'a' && k.text <= 'z';
    char text = (mods & GHOSTTY_MODS_SHIFT) != (letter && e->caps_lock) ? k.shifted : k.text;
    ghostty_key_event_set_action(event, e->action);
    ghostty_key_event_set_key(event, k.key);
    ghostty_key_event_set_mods(event, mods);
    ghostty_key_event_set_consumed_mods(event, text != k.text ? GHOSTTY_MODS_SHIFT : 0);
    ghostty_key_event_set_utf8(event, text ? &text : NULL, text ? 1 : 0);
    ghostty_key_event_set_unshifted_codepoint(event, (uint32_t)(unsigned char)k.text);
    size_t n = terminal_encode_key(event, out, size);
    ghostty_key_event_free(event);
    return n;
}

size_t keys_read(char* out, size_t size) {
    key_event e;
    size_t n = 0;
    while (n == 0 && xQueueReceive(events, &e, 0) == pdTRUE)
        n = encode(&e, out, size);
    return n;
}
