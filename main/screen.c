// SPDX-License-Identifier: GPL-3.0-or-later
#include "screen.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <bsp/m5stack_tab5.h>
#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <shiroko/port_software.h>
#include <shiroko/shiroko.h>

#include "storage.h"

static const char* TAG = "screen";
static esp_lcd_panel_handle_t panel;
static shr_surface framebuffer;
static shr_framebuffer_driver driver;
static shr_context* context;

// malloc() aligns to 4 bytes, and Shiroko's zstd context needs 8
static void* allocate(void* user, size_t size, size_t align, shr_alloc_kind kind) {
    (void)user, (void)kind;
    return heap_caps_aligned_alloc(align < 8 ? 8 : align, size, MALLOC_CAP_SPIRAM);
}

static void release(void* user, void* pointer, size_t size, size_t align, shr_alloc_kind kind) {
    (void)user, (void)size, (void)align, (void)kind;
    free(pointer);
}

static const shr_allocator allocator = {.alloc = allocate, .free = release};

static shr_status acquire(void* user, shr_surface* out) {
    (void)user;
    *out = framebuffer;
    return SHR_OK;
}

static shr_status present(void* user, const shr_surface* surface, uint64_t frame) {
    (void)user, (void)frame;
    return esp_lcd_panel_draw_bitmap(panel, 0, 0, surface->width, surface->height, surface->pixels) == ESP_OK ? SHR_OK : SHR_E_DEVICE;
}

static void discard(void* user, const shr_surface* surface) {
    (void)user, (void)surface;
}

static uint64_t now_ns(void* user) {
    (void)user;
    return (uint64_t)esp_timer_get_time() * 1000;
}

static shr_status read_package(void* user, uint64_t offset, uint32_t length, void* destination, uint64_t request) {
    (void)request;
    return fseek(user, (long)offset, SEEK_SET) == 0 && fread(destination, 1, length, user) == length ? SHR_OK : SHR_E_IO;
}

static void close_package(void* user) {
    fclose(user);
}

static shr_status open_package(void* user, const char* package, shr_asset_source* out) {
    (void)user;
    char path[96];
    snprintf(path, sizeof path, SD "/.local/share/tab5-ssh/fonts/%s", package);
    FILE* f = fopen(path, "rb");
    if (f == NULL) return errno == ENOENT ? SHR_E_NOT_FOUND : SHR_E_IO;
    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        fclose(f);
        return SHR_E_IO;
    }
    shr_asset_source_init(out);
    out->user = f;
    out->size = (uint64_t)st.st_size;
    out->read = read_package;
    out->close = close_package;
    return SHR_OK;
}

static void draw(void) {
    shr_deadline next;
    shr_event event;
    for (shr_submit(context);;) {
        shr_pump(context);
        while (shr_poll_event(context, &event) == SHR_OK)
            if (event.kind == SHR_EVENT_PRESENT_FAILED || event.kind == SHR_EVENT_RESOURCE_FAILED)
                ESP_LOGW(TAG, "event %d: %s", event.kind, shr_status_name(event.status));
        if (shr_next_deadline(context, &next) != SHR_OK || next.kind == SHR_DEADLINE_NONE) return;
        if (next.kind == SHR_DEADLINE_AT) vTaskDelay(1);
    }
}

static shr_status check(shr_status status, const char* what) {
    if (status != SHR_OK) ESP_LOGE(TAG, "%s: %s", what, shr_status_name(status));
    return status;
}

esp_err_t screen_start(const global_state* state) {
    bsp_lcd_handles_t lcd;
    void* pixels;
    ESP_RETURN_ON_ERROR(bsp_display_new_with_handles(&(bsp_display_config_t){.dsi_bus.lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS}, &lcd),
                        TAG, "panel");
    panel = lcd.panel;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "panel on");
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &pixels), TAG, "frame buffer");
    ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "backlight");
    framebuffer = (shr_surface){.pixels = pixels,
                                .width = BSP_LCD_H_RES,
                                .height = BSP_LCD_V_RES,
                                .stride = BSP_LCD_H_RES * 2,
                                .byte_length = BSP_LCD_H_RES * 2 * BSP_LCD_V_RES,
                                .format = SHR_FORMAT_RGB565};

    shr_output output;
    shr_output_init(&output);
    output.acquire = acquire, output.present = present, output.discard = discard;
    output.flags = SHR_OUTPUT_RELEASE_ON_PRESENT | SHR_OUTPUT_PRESERVES_CONTENT;
    shr_context_desc cd;
    shr_context_desc_init(&cd);
    cd.allocator = &allocator, cd.driver = &driver, cd.output = &output, cd.now_ns = now_ns;
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = BSP_LCD_V_RES, sd.height = BSP_LCD_H_RES, sd.rotation = SHR_ROTATE_90_CCW, sd.clear = SHR_RGB(0, 0, 0);
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.open = open_package, fd.locale = "ko";
    shr_pl_res_bitmap_font* font;
    shr_lyr* layer;
    const terminal_size* size = &state->terminal_size;
    shr_color background = SHR_RGB(0, 0, 0);
    static const char text[] = "Tab5 SSH client\n"
                               " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~\n"
                               "다람쥐 헌 쳇바퀴에 타고파";
    if (check(shr_software_driver_create(&allocator, 0, 0, 256, &driver), "driver") != SHR_OK ||
        check(shr_create(&cd, &context), "context") != SHR_OK || check(shr_screen_configure(context, &sd), "screen") != SHR_OK ||
        check(shr_pl_res_bitmap_font_create(context, &fd, &font), "font") != SHR_OK ||
        check(shr_lyr_create(context, 0, (shr_rect){0, 0, (int32_t)size->width_pixels, (int32_t)size->height_pixels}, &layer), "layer") != SHR_OK ||
        check(shr_pl_lyr_tilemap_resize(layer, font, (int32_t)size->rows, (int32_t)size->columns, &background), "tilemap") != SHR_OK ||
        check(shr_pl_lyr_tilemap_set_text(layer, 0, 0, text, sizeof text - 1, (shr_text_style){.fg = SHR_RGB(0xff, 0xff, 0xff)}, NULL, 0,
                                          SHR_TEXT_WRAP, NULL),
              "text") != SHR_OK)
        return ESP_FAIL;
    draw();
    return ESP_OK;
}
