// SPDX-License-Identifier: GPL-3.0-or-later
#include "screen.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <bsp/m5stack_tab5.h>
#include <esp_check.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <shiroko/port_software.h>
#include <shiroko/shiroko.h>

#include "storage.h"
#include "terminal.h"

static const char* TAG = "screen";
static esp_lcd_panel_handle_t panel;
static shr_surface framebuffer;
static shr_framebuffer_driver driver;
static shr_context* context;
static shr_lyr *grid, *cursor;
static GhosttyRenderStateRowIterator rows;
static GhosttyRenderStateRowCells cells;
static GhosttyRenderStateCursor shown;

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
    cd.driver = &driver, cd.output = &output, cd.now_ns = now_ns;
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = BSP_LCD_V_RES, sd.height = BSP_LCD_H_RES, sd.rotation = SHR_ROTATE_90_CCW, sd.clear = SHR_RGB(0, 0, 0);
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.open = open_package, fd.locale = "ko";
    shr_pl_res_bitmap_font* font;
    const terminal_size* size = &state->terminal_size;
    shr_rect area = {0, 0, (int32_t)size->width_pixels, (int32_t)size->height_pixels};
    shr_color background = SHR_RGB(0, 0, 0);
    if (check(shr_software_driver_create(NULL, 0, 0, 256, &driver), "driver") != SHR_OK || check(shr_create(&cd, &context), "context") != SHR_OK ||
        check(shr_screen_configure(context, &sd), "screen") != SHR_OK ||
        check(shr_pl_res_bitmap_font_create(context, &fd, &font), "font") != SHR_OK ||
        check(shr_lyr_create(context, 0, area, &grid), "grid") != SHR_OK || check(shr_lyr_create(context, 1, area, &cursor), "cursor") != SHR_OK ||
        check(shr_pl_lyr_tilemap_resize(grid, font, (int32_t)size->rows, (int32_t)size->columns, &background), "tilemap") != SHR_OK ||
        ghostty_render_state_row_iterator_new(&terminal_allocator, &rows) != GHOSTTY_SUCCESS ||
        ghostty_render_state_row_cells_new(&terminal_allocator, &cells) != GHOSTTY_SUCCESS)
        return ESP_FAIL;
    return ESP_OK;
}

static shr_color rgb(GhosttyColorRgb c) {
    return SHR_RGB(c.r, c.g, c.b);
}

static void draw_cell(int32_t y, int32_t x, const GhosttyRenderStateColors* colors) {
    GhosttyCell raw;
    GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
    GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
    GhosttyColorRgb fg = colors->foreground, bg = colors->background;
    char text[64];
    GhosttyBuffer utf8 = {(uint8_t*)text, sizeof text, 0};
    ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw);
    ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide);
    if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) return;
    ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
    ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg);
    bool painted = ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg) == GHOSTTY_SUCCESS;
    if (ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &utf8) != GHOSTTY_SUCCESS) utf8.len = 0;
    if (style.inverse) {
        GhosttyColorRgb swap = fg;
        fg = bg, bg = swap, painted = true;
    }
    // fg alpha: 255 full, 128 dim, 0 none; bg alpha 0 leaves the tilemap's background
    shr_text_style cell = {.fg = (rgb(fg) & 0xffffffu) | (style.invisible ? 0
                                                          : style.faint   ? 0x80000000u
                                                                          : 0xff000000u),
                           .bg = painted ? rgb(bg) : 0,
                           .flags = (style.bold ? SHR_STYLE_BOLD : 0) | (style.italic ? SHR_STYLE_ITALIC : 0)};
    shr_pl_lyr_tilemap_set_cell(grid, y, x, text, utf8.len, wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : 1, cell);
}

static void draw_cursor(const GhosttyRenderStateCursor* c, const GhosttyRenderStateColors* colors) {
    shr_lyr_cmd_begin(cursor);
    if (c->visible && c->viewport_has_value) {
        int32_t x = (c->viewport_x - c->wide_tail) * SHR_CELL_WIDTH, y = c->viewport_y * SHR_CELL_HEIGHT;
        shr_rect r = {x, y, x + SHR_CELL_WIDTH, y + SHR_CELL_HEIGHT};
        if (c->visual_style == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR) r.x1 = x + 2;
        if (c->visual_style == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE) r.y0 = r.y1 - 2;
        shr_lyr_cmd_fill(cursor, r, rgb(colors->cursor_has_value ? colors->cursor : colors->foreground));
    }
    shr_lyr_cmd_commit(cursor);
}

void screen_draw(void) {
    GhosttyRenderState render = terminal_update();
    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    GhosttyRenderStateCursor now = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_COLORS, &colors);
    ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_CURSOR, &now);
    GhosttyRenderStateRowMoves moves = {0};
    ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_ROW_MOVES, &moves);
    for (int32_t i = 0; i < moves.count; i++)
        shr_pl_lyr_tilemap_scroll(grid, moves.moves[i].top, moves.moves[i].bottom, moves.moves[i].n, (shr_text_style){0});
    ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, (void*)&rows);
    for (uint16_t y; ghostty_render_state_row_iterator_next_dirty(rows, &y);) {
        ghostty_render_state_row_get(rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, (void*)&cells);
        for (int32_t x = 0; ghostty_render_state_row_cells_next(cells); x++)
            draw_cell(y, x, &colors);
    }
    ghostty_render_state_clean(render);
    if (now.visible != shown.visible || now.viewport_has_value != shown.viewport_has_value || now.viewport_x != shown.viewport_x ||
        now.viewport_y != shown.viewport_y || now.wide_tail != shown.wide_tail || now.visual_style != shown.visual_style)
        draw_cursor(&now, &colors);
    shown = now;

    shr_deadline next;
    shr_event event;
    for (shr_submit(context);;) {
        shr_pump(context);
        while (shr_poll_event(context, &event) == SHR_OK)
            if (event.kind == SHR_EVENT_PRESENT_FAILED || event.kind == SHR_EVENT_RESOURCE_FAILED)
                ESP_LOGW(TAG, "event %d: %s", event.kind, shr_status_name(event.status));
        if (shr_next_deadline(context, &next) != SHR_OK || next.kind != SHR_DEADLINE_NOW) return;
    }
}
