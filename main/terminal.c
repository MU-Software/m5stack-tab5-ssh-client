// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal.h"

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
static GhosttyTerminal terminal;
static GhosttyRenderState render;
static SemaphoreHandle_t lock;
static uint8_t replies[256];
static size_t reply_size;

static void reply(GhosttyTerminal t, void* user, const uint8_t* data, size_t size) {
    (void)t, (void)user;
    if (size > sizeof replies - reply_size) return;
    memcpy(replies + reply_size, data, size);
    reply_size += size;
}

esp_err_t terminal_start(const terminal_size* size) {
    lock = xSemaphoreCreateMutex();
    if (lock == NULL || ghostty_terminal_new(&terminal_allocator, &terminal, (uint16_t)size->columns, (uint16_t)size->rows) != GHOSTTY_SUCCESS ||
        ghostty_render_state_new(&terminal_allocator, &render) != GHOSTTY_SUCCESS)
        return ESP_ERR_NO_MEM;
    GhosttyTerminalModeConfig graphemes = {GHOSTTY_MODE_GRAPHEME_CLUSTER, true};
    uint64_t image_bytes = 4 << 20;
    size_t command_bytes = 8 << 20; // instead of 65 MiB
    ghostty_terminal_resize(terminal, (uint16_t)size->columns, (uint16_t)size->rows, size->width_pixels / size->columns,
                            size->height_pixels / size->rows);
    // Without defaults, ?5h and OSC 10/11 do nothing
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &(GhosttyColorRgb){0xff, 0xff, 0xff});
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &(GhosttyColorRgb){0, 0, 0});
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_MODE_DEFAULT, &graphemes);
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT, &image_bytes);
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_APC_MAX_BYTES_KITTY, &command_bytes);
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY, (const void*)reply);
    ghostty_render_state_set(render, GHOSTTY_RENDER_STATE_OPTION_ROW_MOVES, &(bool){true});
    return ESP_OK;
}

void terminal_write(const void* data, size_t size) {
    xSemaphoreTake(lock, portMAX_DELAY);
    ghostty_terminal_vt_write(terminal, data, size);
    xSemaphoreGive(lock);
}

size_t terminal_reply(void* buffer, size_t size) {
    size_t n = reply_size < size ? reply_size : size;
    memcpy(buffer, replies, n);
    memmove(replies, replies + n, reply_size - n);
    reply_size -= n;
    return n;
}

GhosttyRenderState terminal_update(void (*images)(GhosttyTerminal terminal, GhosttyKittyGraphics graphics)) {
    GhosttyKittyGraphics graphics;
    xSemaphoreTake(lock, portMAX_DELAY);
    ghostty_render_state_update(render, terminal);
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, (void*)&graphics) == GHOSTTY_SUCCESS) images(terminal, graphics);
    xSemaphoreGive(lock);
    return render;
}
