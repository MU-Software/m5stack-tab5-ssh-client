// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal.h"

#include <stdatomic.h>
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
    SemaphoreHandle_t lock;
    uint8_t replies[256];
    size_t reply_size;
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
        ghostty_key_encoder_new(&terminal_allocator, &t->encoder) != GHOSTTY_SUCCESS) {
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
    ghostty_key_encoder_free(t->encoder);
    ghostty_render_state_free(t->render);
    ghostty_terminal_free(t->vt);
    if (t->lock != NULL) vSemaphoreDelete(t->lock);
    heap_caps_free(t);
}

void terminal_resize(terminal* t, const terminal_size* size) {
    xSemaphoreTake(t->lock, portMAX_DELAY);
    ghostty_terminal_resize(t->vt, (uint16_t)size->columns, (uint16_t)size->rows, size->width_pixels / size->columns,
                            size->height_pixels / size->rows);
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
