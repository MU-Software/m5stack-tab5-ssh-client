// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static void* allocate(void* context, size_t size, uint8_t alignment, uintptr_t caller) {
    (void)context, (void)caller;
    return heap_caps_aligned_alloc((size_t)1 << alignment, size, MALLOC_CAP_SPIRAM);
}

static bool resize(void* context, void* memory, size_t size, uint8_t alignment, size_t new_size, uintptr_t caller) {
    (void)context, (void)memory, (void)alignment, (void)caller;
    return new_size <= size;
}

static void* remap(void* context, void* memory, size_t size, uint8_t alignment, size_t new_size, uintptr_t caller) {
    (void)context, (void)memory, (void)size, (void)alignment, (void)new_size, (void)caller;
    return NULL;
}

static void release(void* context, void* memory, size_t size, uint8_t alignment, uintptr_t caller) {
    (void)context, (void)size, (void)alignment, (void)caller;
    heap_caps_free(memory);
}

const GhosttyAllocator terminal_allocator = {.vtable = &(const GhosttyAllocatorVtable){allocate, resize, remap, release}};

struct terminal {
    GhosttyTerminal vt;
    GhosttyRenderState render;
    GhosttyKeyEncoder encoder;
    GhosttyMouseEncoder mouse;
    GhosttyMouseEvent mouse_event;
    SemaphoreHandle_t lock;
    uint8_t replies[256];
    size_t reply_size;
    // terminal_read_mouse's, written by the render task under the lock
    uint8_t reports[256];
    size_t report_size;
};

static _Atomic(terminal*) shown;

static void reply(GhosttyTerminal vt, void* user, const uint8_t* data, size_t size) {
    terminal* t = user;
    (void)vt;
    if (size > sizeof t->replies - t->reply_size) return;
    memcpy(t->replies + t->reply_size, data, size);
    t->reply_size += size;
}

terminal* terminal_new(const terminal_size* size) {
    terminal* t = heap_caps_calloc(1, sizeof *t, MALLOC_CAP_SPIRAM);
    if (t == NULL || (t->lock = xSemaphoreCreateMutex()) == NULL ||
        ghostty_terminal_new(&terminal_allocator, &t->vt, (uint16_t)size->columns, (uint16_t)size->rows) != GHOSTTY_SUCCESS ||
        ghostty_render_state_new(&terminal_allocator, &t->render) != GHOSTTY_SUCCESS ||
        ghostty_key_encoder_new(&terminal_allocator, &t->encoder) != GHOSTTY_SUCCESS ||
        ghostty_mouse_encoder_new(&terminal_allocator, &t->mouse) != GHOSTTY_SUCCESS ||
        ghostty_mouse_event_new(&terminal_allocator, &t->mouse_event) != GHOSTTY_SUCCESS) {
        terminal_free(t);
        return NULL;
    }
    GhosttyTerminalModeConfig graphemes = {GHOSTTY_MODE_GRAPHEME_CLUSTER, true};
    uint64_t image_bytes = 4 << 20;
    size_t command_bytes = 8 << 20; // instead of 65 MiB
    terminal_resize(t, size);
    // Without defaults, ?5h and OSC 10/11 do nothing
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &(GhosttyColorRgb){0xff, 0xff, 0xff});
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &(GhosttyColorRgb){0, 0, 0});
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_MODE_DEFAULT, &graphemes);
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT, &image_bytes);
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_APC_MAX_BYTES_KITTY, &command_bytes);
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_USERDATA, t);
    ghostty_terminal_set(t->vt, GHOSTTY_TERMINAL_OPT_WRITE_PTY, (const void*)reply);
    ghostty_render_state_set(t->render, GHOSTTY_RENDER_STATE_OPTION_ROW_MOVES, &(bool){true});
    return t;
}

void terminal_free(terminal* t) {
    if (t == NULL) return;
    ghostty_mouse_event_free(t->mouse_event);
    ghostty_mouse_encoder_free(t->mouse);
    ghostty_key_encoder_free(t->encoder);
    ghostty_render_state_free(t->render);
    ghostty_terminal_free(t->vt);
    if (t->lock != NULL) vSemaphoreDelete(t->lock);
    heap_caps_free(t);
}

void terminal_resize(terminal* t, const terminal_size* size) {
    GhosttyMouseEncoderSize pixels = GHOSTTY_INIT_SIZED(GhosttyMouseEncoderSize);
    pixels.screen_width = size->width_pixels, pixels.screen_height = size->height_pixels;
    pixels.cell_width = size->width_pixels / size->columns, pixels.cell_height = size->height_pixels / size->rows;
    xSemaphoreTake(t->lock, portMAX_DELAY);
    ghostty_terminal_resize(t->vt, (uint16_t)size->columns, (uint16_t)size->rows, pixels.cell_width, pixels.cell_height);
    ghostty_mouse_encoder_setopt(t->mouse, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &pixels);
    xSemaphoreGive(t->lock);
}

void terminal_show(terminal* t) {
    atomic_store(&shown, t);
}

void terminal_write(terminal* t, const void* data, size_t size) {
    xSemaphoreTake(t->lock, portMAX_DELAY);
    ghostty_terminal_vt_write(t->vt, data, size);
    xSemaphoreGive(t->lock);
}

size_t terminal_encode_key(GhosttyKeyEvent event, char* out, size_t size) {
    terminal* t = atomic_load(&shown);
    size_t n = 0;
    if (t == NULL) return 0;
    xSemaphoreTake(t->lock, portMAX_DELAY);
    ghostty_key_encoder_setopt_from_terminal(t->encoder, t->vt);
    if (ghostty_key_encoder_encode(t->encoder, event, out, size, &n) != GHOSTTY_SUCCESS) n = 0;
    xSemaphoreGive(t->lock);
    return n;
}

// libghostty-vt's Zig takes the position's two floats in integer registers, where GCC passes such a struct in float registers
void set_position_words(GhosttyMouseEvent event, uint32_t x, uint32_t y) __asm__("ghostty_mouse_event_set_position");
static void set_position(GhosttyMouseEvent event, float x, float y) {
    uint32_t words[2];
    memcpy(words, (float[]){x, y}, sizeof words);
    set_position_words(event, words[0], words[1]);
}

// Under the lock
static void add_report(terminal* t, const char* bytes, size_t size) {
    if (size <= sizeof t->reports - t->report_size) memcpy(t->reports + t->report_size, bytes, size), t->report_size += size;
}

// Under the lock, after ghostty_mouse_encoder_setopt_from_terminal: false when the program asked for no such report
static bool report(terminal* t, GhosttyMouseAction action, GhosttyMouseButton button, int32_t x, int32_t y) {
    char out[32];
    size_t n = 0;
    ghostty_mouse_event_set_action(t->mouse_event, action);
    ghostty_mouse_event_set_button(t->mouse_event, button);
    set_position(t->mouse_event, (float)x, (float)y);
    ghostty_mouse_encoder_setopt(t->mouse, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED,
                                 &(bool){action == GHOSTTY_MOUSE_ACTION_PRESS && button == GHOSTTY_MOUSE_BUTTON_LEFT});
    if (ghostty_mouse_encoder_encode(t->mouse, t->mouse_event, out, sizeof out, &n) != GHOSTTY_SUCCESS || n == 0) return false;
    add_report(t, out, n);
    return true;
}

// Under the lock: as Ghostty does for mode 1007 (alternate scroll) on the alternate screen; false elsewhere
static bool arrow_keys(terminal* t, int32_t lines) {
    GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
    GhosttyTerminalModeConfig alternate_scroll = {GHOSTTY_MODE_ALT_SCROLL, false};
    GhosttyKeyEvent event;
    char out[16];
    size_t n = 0;
    ghostty_terminal_get(t->vt, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen);
    ghostty_terminal_get(t->vt, GHOSTTY_TERMINAL_DATA_MODE, &alternate_scroll);
    if (screen != GHOSTTY_TERMINAL_SCREEN_ALTERNATE || !alternate_scroll.value ||
        ghostty_key_event_new(&terminal_allocator, &event) != GHOSTTY_SUCCESS)
        return false;
    ghostty_key_event_set_action(event, GHOSTTY_KEY_ACTION_PRESS);
    ghostty_key_event_set_key(event, lines < 0 ? GHOSTTY_KEY_ARROW_UP : GHOSTTY_KEY_ARROW_DOWN);
    ghostty_key_encoder_setopt_from_terminal(t->encoder, t->vt);
    if (ghostty_key_encoder_encode(t->encoder, event, out, sizeof out, &n) == GHOSTTY_SUCCESS)
        for (int32_t i = 0; i < abs(lines); i++)
            add_report(t, out, n);
    ghostty_key_event_free(event);
    return true;
}

void terminal_click(terminal* t, int32_t x, int32_t y) {
    xSemaphoreTake(t->lock, portMAX_DELAY);
    ghostty_mouse_encoder_setopt_from_terminal(t->mouse, t->vt);
    if (report(t, GHOSTTY_MOUSE_ACTION_PRESS, GHOSTTY_MOUSE_BUTTON_LEFT, x, y))
        report(t, GHOSTTY_MOUSE_ACTION_RELEASE, GHOSTTY_MOUSE_BUTTON_LEFT, x, y);
    xSemaphoreGive(t->lock);
}

// Wheel buttons are pressed only, as in xterm
void terminal_wheel(terminal* t, int32_t lines, int32_t x, int32_t y, bool arrows) {
    GhosttyMouseButton button = lines < 0 ? GHOSTTY_MOUSE_BUTTON_FOUR : GHOSTTY_MOUSE_BUTTON_FIVE;
    xSemaphoreTake(t->lock, portMAX_DELAY);
    ghostty_mouse_encoder_setopt_from_terminal(t->mouse, t->vt);
    if (report(t, GHOSTTY_MOUSE_ACTION_PRESS, button, x, y))
        for (int32_t i = 1; i < abs(lines); i++)
            report(t, GHOSTTY_MOUSE_ACTION_PRESS, button, x, y);
    else if (!arrows || !arrow_keys(t, lines))
        ghostty_terminal_scroll_viewport(t->vt, (GhosttyTerminalScrollViewport){.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA, .value.delta = lines});
    xSemaphoreGive(t->lock);
}

void terminal_scroll_to_bottom(terminal* t) {
    bool bottom = true;
    xSemaphoreTake(t->lock, portMAX_DELAY);
    ghostty_terminal_get(t->vt, GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE, &bottom);
    if (!bottom) ghostty_terminal_scroll_viewport(t->vt, (GhosttyTerminalScrollViewport){.tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM});
    xSemaphoreGive(t->lock);
}

size_t terminal_read_mouse(terminal* t, void* buffer, size_t size) {
    xSemaphoreTake(t->lock, portMAX_DELAY);
    size_t n = t->report_size < size ? t->report_size : size;
    memcpy(buffer, t->reports, n);
    memmove(t->reports, t->reports + n, t->report_size - n);
    t->report_size -= n;
    xSemaphoreGive(t->lock);
    return n;
}

size_t terminal_reply(terminal* t, void* buffer, size_t size) {
    size_t n = t->reply_size < size ? t->reply_size : size;
    memcpy(buffer, t->replies, n);
    memmove(t->replies, t->replies + n, t->reply_size - n);
    t->reply_size -= n;
    return n;
}

GhosttyRenderState terminal_update(terminal* t, void (*images)(GhosttyTerminal terminal, GhosttyKittyGraphics graphics)) {
    GhosttyKittyGraphics graphics;
    xSemaphoreTake(t->lock, portMAX_DELAY);
    ghostty_render_state_update(t->render, t->vt);
    if (ghostty_terminal_get(t->vt, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, (void*)&graphics) == GHOSTTY_SUCCESS) images(t->vt, graphics);
    xSemaphoreGive(t->lock);
    return t->render;
}
