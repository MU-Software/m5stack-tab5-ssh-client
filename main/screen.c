// SPDX-License-Identifier: GPL-3.0-or-later
#include "screen.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <bsp/m5stack_tab5.h>
#include <driver/ppa.h>
#include <esp_cache.h>
#include <esp_check.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <shiroko/port_software.h>
#include <shiroko/shiroko.h>

#include "keys.h"
#include "storage.h"
#include "terminal.h"

static const char* TAG = "screen";
static esp_lcd_panel_handle_t panel;
static shr_surface framebuffer;
static shr_framebuffer_driver software, driver;
static ppa_client_handle_t ppa;
static uint8_t band[BSP_LCD_V_RES * 2 * 16] __attribute__((aligned(CONFIG_CACHE_L2_CACHE_LINE_SIZE)));
static shr_context* context;
static shr_lyr *grid, *pictures, *cursor, *composing;
static GhosttyKittyGraphicsPlacementIterator placements;

// A kitty image as drawn: its source rect scaled to width x height
typedef struct {
    uint32_t id, source_x, source_y, source_width, source_height, width, height;
    uint64_t generation;
    shr_pl_res_image* image;
    bool used;
} picture;

typedef struct {
    shr_pl_res_image* image;
    shr_point at;
    int32_t width, height, z;
} placed;

static picture made[16];
static placed shown[16], wanted[16];
static int shown_count, wanted_count;
static GhosttyRenderStateRowIterator rows;
static GhosttyRenderStateRowCells cells;
static GhosttyRenderStateCursor shown_cursor;

static shr_status acquire(void* user, shr_surface* out) {
    (void)user;
    *out = framebuffer;
    return SHR_OK;
}

static shr_status present(void* user, const shr_surface* surface, uint64_t frame) {
    (void)user, (void)frame;
    return esp_lcd_panel_draw_bitmap(panel, 0, 0, surface->width, surface->height, surface->pixels) == ESP_OK ? SHR_OK : SHR_E_DEVICE;
}

// Band parts go to the portrait panel through the PPA; offsets of 2 mod 4 can hang it (esp-idf#19096), hence 16
static bool rotate(const shr_surface* dst, const shr_draw_cmd* cmds, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (cmds[i].kind != SHR_CMD_ROTATE || cmds[i].rotation != SHR_ROTATE_90_CCW || (cmds[i].dst.x0 | cmds[i].dst.y0) % 16 != 0 ||
            (const uint8_t*)cmds[i].src.pixels < band || (const uint8_t*)cmds[i].src.pixels >= band + sizeof band)
            return false;
    for (size_t i = 0; i < count; i++) {
        const shr_draw_cmd* c = &cmds[i];
        size_t offset = (size_t)((const uint8_t*)c->src.pixels - band), stride = BSP_LCD_V_RES * 2;
        ppa_srm_oper_config_t operation = {.in = {.buffer = band,
                                                  .pic_w = BSP_LCD_V_RES,
                                                  .pic_h = 16,
                                                  .block_w = c->src.width,
                                                  .block_h = c->src.height,
                                                  .block_offset_x = offset % stride / 2,
                                                  .block_offset_y = offset / stride,
                                                  .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
                                           .out = {.buffer = dst->pixels,
                                                   .buffer_size = dst->byte_length,
                                                   .pic_w = dst->width,
                                                   .pic_h = dst->height,
                                                   .block_offset_x = c->dst.x0,
                                                   .block_offset_y = c->dst.y0,
                                                   .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
                                           .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
                                           .scale_x = 1,
                                           .scale_y = 1,
                                           .mode = PPA_TRANS_MODE_BLOCKING};
        if (ppa_do_scale_rotate_mirror(ppa, &operation) != ESP_OK) return false;
    }
    return true;
}

static shr_status execute(void* user, const shr_surface* dst, const shr_draw_cmd* cmds, size_t count, shr_fence fence) {
    (void)user;
    if (rotate(dst, cmds, count)) return SHR_OK;
    shr_status status = software.execute(software.user, dst, cmds, count, fence);
    // The PPA invalidates the panel's cache lines before writing
    if (dst->pixels == framebuffer.pixels) esp_cache_msync(dst->pixels, dst->byte_length, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    return status;
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
    shr_surface bands = {
        .pixels = band, .width = BSP_LCD_V_RES, .height = 16, .stride = BSP_LCD_V_RES * 2, .byte_length = sizeof band, .format = SHR_FORMAT_RGB565};
    sd.bands = &bands, sd.band_count = 1, sd.band_align = 16;
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.open = open_package, fd.locale = "ko";
    shr_pl_res_bitmap_font* font;
    const terminal_size* size = &state->terminal_size;
    shr_rect area = {0, 0, (int32_t)size->width_pixels, (int32_t)size->height_pixels};
    shr_color background = SHR_RGB(0, 0, 0);
    ESP_RETURN_ON_ERROR(ppa_register_client(&(ppa_client_config_t){.oper_type = PPA_OPERATION_SRM}, &ppa), TAG, "PPA");
    if (check(shr_software_driver_create(NULL, 0, 0, 256, &software), "driver") != SHR_OK) return ESP_FAIL;
    // Translucent images: 8 bytes a pixel in planes, 8 MiB by default
    shr_software_driver_image_planes(&software, 2 << 20);
    driver = software, driver.execute = execute;
    // Moves would be CPU memmoves in PSRAM: 33 ms per one-line scroll against 21 ms for drawing the rows again
    driver.caps.flags &= ~SHR_DRIVER_CHEAP_MOVE;
    if (check(shr_create(&cd, &context), "context") != SHR_OK || check(shr_screen_configure(context, &sd), "screen") != SHR_OK ||
        check(shr_pl_res_bitmap_font_create(context, &fd, &font), "font") != SHR_OK ||
        check(shr_lyr_create(context, 0, area, &grid), "grid") != SHR_OK ||
        check(shr_lyr_create(context, 1, area, &pictures), "pictures") != SHR_OK ||
        check(shr_lyr_create(context, 2, area, &cursor), "cursor") != SHR_OK ||
        check(shr_lyr_create(context, 3, (shr_rect){0, 0, 2 * SHR_CELL_WIDTH, SHR_CELL_HEIGHT}, &composing), "composing") != SHR_OK ||
        check(shr_pl_lyr_tilemap_resize(composing, font, 1, 2, NULL), "composing") != SHR_OK ||
        check(shr_pl_lyr_tilemap_resize(grid, font, (int32_t)size->rows, (int32_t)size->columns, &background), "tilemap") != SHR_OK ||
        ghostty_render_state_row_iterator_new(&terminal_allocator, &rows) != GHOSTTY_SUCCESS ||
        ghostty_render_state_row_cells_new(&terminal_allocator, &cells) != GHOSTTY_SUCCESS ||
        ghostty_kitty_graphics_placement_iterator_new(&terminal_allocator, &placements) != GHOSTTY_SUCCESS)
        return ESP_FAIL;
    return ESP_OK;
}

static picture* make_picture(GhosttyKittyGraphicsImage image, uint32_t id, const GhosttyKittyGraphicsPlacementRenderInfo* r) {
    static const int sizes[] = {[GHOSTTY_KITTY_IMAGE_FORMAT_RGB] = 3,
                                [GHOSTTY_KITTY_IMAGE_FORMAT_RGBA] = 4,
                                [GHOSTTY_KITTY_IMAGE_FORMAT_GRAY_ALPHA] = 2,
                                [GHOSTTY_KITTY_IMAGE_FORMAT_GRAY] = 1};
    static const shr_image_source_format formats[] = {[GHOSTTY_KITTY_IMAGE_FORMAT_RGB] = SHR_IMAGE_SRC_RGB888,
                                                      [GHOSTTY_KITTY_IMAGE_FORMAT_RGBA] = SHR_IMAGE_SRC_RGBA8888,
                                                      [GHOSTTY_KITTY_IMAGE_FORMAT_GRAY_ALPHA] = SHR_IMAGE_SRC_GRAY_ALPHA88,
                                                      [GHOSTTY_KITTY_IMAGE_FORMAT_GRAY] = SHR_IMAGE_SRC_GRAY8};
    uint64_t generation = 0;
    uint32_t width = 0, height = 0;
    GhosttyKittyImageFormat format = GHOSTTY_KITTY_IMAGE_FORMAT_PNG;
    const uint8_t* pixels = NULL;
    ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_GENERATION, &generation);
    picture key = {id, r->source_x, r->source_y, r->source_width, r->source_height, r->pixel_width, r->pixel_height, generation, NULL, true};
    picture* free_slot = NULL;
    for (size_t i = 0; i < sizeof made / sizeof made[0]; i++) {
        picture* p = &made[i];
        if (p->image != NULL && p->id == id && p->generation == generation && p->source_x == key.source_x && p->source_y == key.source_y &&
            p->source_width == key.source_width && p->source_height == key.source_height && p->width == key.width && p->height == key.height) {
            p->used = true;
            return p;
        }
        if (p->image == NULL && free_slot == NULL) free_slot = p;
    }
    ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_WIDTH, &width);
    ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_HEIGHT, &height);
    ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_FORMAT, &format);
    if (free_slot == NULL || format == GHOSTTY_KITTY_IMAGE_FORMAT_PNG || format > GHOSTTY_KITTY_IMAGE_FORMAT_GRAY ||
        ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR, (void*)&pixels) != GHOSTTY_SUCCESS)
        return NULL;
    shr_image_source source = {(int32_t)width, (int32_t)height, formats[format], pixels, (size_t)width * sizes[format]};
    shr_rect area = {(int32_t)key.source_x, (int32_t)key.source_y, (int32_t)(key.source_x + key.source_width),
                     (int32_t)(key.source_y + key.source_height)};
    shr_status status;
    if (key.width == key.source_width && key.height == key.source_height) {
        source.pixels = pixels + key.source_y * source.stride + key.source_x * sizes[format];
        source.width = (int32_t)key.width, source.height = (int32_t)key.height;
        status = shr_pl_res_image_create_from(context, &source, &key.image);
    } else {
        uint32_t filter = key.width < key.source_width || key.height < key.source_height ? SHR_SCALE_BOX : SHR_SCALE_BILINEAR;
        status = shr_pl_res_image_create_scaled(context, &source, area, (int32_t)key.width, (int32_t)key.height, filter, &key.image);
    }
    if (status != SHR_OK) return NULL;
    *free_slot = key;
    return free_slot;
}

// Under the terminal's lock: its kitty images and their places in this frame; z < 0 would be under the opaque cells
static void find_pictures(GhosttyTerminal terminal, GhosttyKittyGraphics graphics) {
    uint64_t generation = 0;
    wanted_count = 0;
    for (size_t i = 0; i < sizeof made / sizeof made[0]; i++)
        made[i].used = false;
    if (ghostty_kitty_graphics_get(graphics, GHOSTTY_KITTY_GRAPHICS_DATA_GENERATION, &generation) != GHOSTTY_SUCCESS || generation == 0 ||
        ghostty_kitty_graphics_get(graphics, GHOSTTY_KITTY_GRAPHICS_DATA_PLACEMENT_ITERATOR, (void*)&placements) != GHOSTTY_SUCCESS)
        return;
    while (wanted_count < (int)(sizeof wanted / sizeof wanted[0]) && ghostty_kitty_graphics_placement_next(placements)) {
        uint32_t id = 0, x = 0, y = 0;
        int32_t z = 0;
        GhosttyKittyGraphicsPlacementRenderInfo r = GHOSTTY_INIT_SIZED(GhosttyKittyGraphicsPlacementRenderInfo);
        ghostty_kitty_graphics_placement_get(placements, GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_IMAGE_ID, &id);
        ghostty_kitty_graphics_placement_get(placements, GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_Z, &z);
        ghostty_kitty_graphics_placement_get(placements, GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_X_OFFSET, &x);
        ghostty_kitty_graphics_placement_get(placements, GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_Y_OFFSET, &y);
        GhosttyKittyGraphicsImage image = ghostty_kitty_graphics_image(graphics, id);
        picture* p;
        if (z < 0 || image == NULL || ghostty_kitty_graphics_placement_render_info(placements, image, terminal, &r) != GHOSTTY_SUCCESS ||
            !r.viewport_visible || r.pixel_width == 0 || r.pixel_height == 0 || (p = make_picture(image, id, &r)) == NULL)
            continue;
        placed e = {p->image,
                    {r.viewport_col * SHR_CELL_WIDTH + (int32_t)x, r.viewport_row * SHR_CELL_HEIGHT + (int32_t)y},
                    (int32_t)p->width,
                    (int32_t)p->height,
                    z};
        int i = wanted_count++;
        for (; i > 0 && wanted[i - 1].z > z; i--)
            wanted[i] = wanted[i - 1];
        wanted[i] = e;
    }
}

static void draw_pictures(void) {
    if (wanted_count == shown_count && memcmp(wanted, shown, sizeof wanted[0] * (size_t)wanted_count) == 0) return;
    shr_lyr_cmd_begin(pictures);
    for (int i = 0; i < wanted_count; i++) {
        const placed* e = &wanted[i];
        shr_rect src = {e->at.x < 0 ? -e->at.x : 0, e->at.y < 0 ? -e->at.y : 0, e->width, e->height};
        shr_lyr_cmd_image(pictures, e->image, src, (shr_point){e->at.x < 0 ? 0 : e->at.x, e->at.y < 0 ? 0 : e->at.y});
    }
    shr_lyr_cmd_commit(pictures);
    memcpy(shown, wanted, sizeof wanted);
    shown_count = wanted_count;
    for (size_t k = 0; k < sizeof made / sizeof made[0]; k++)
        if (made[k].image != NULL && !made[k].used) {
            shr_pl_res_image_release(made[k].image);
            made[k].image = NULL;
        }
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

// The Korean syllable being typed, over the cursor's cell
static void draw_composing(const GhosttyRenderStateCursor* c, const GhosttyRenderStateColors* colors) {
    static uint32_t shown_text;
    static int32_t shown_x, shown_y;
    uint32_t p = keys_preedit();
    int32_t x = c->viewport_x * SHR_CELL_WIDTH, y = c->viewport_y * SHR_CELL_HEIGHT;
    if (p == shown_text && (p == 0 || (x == shown_x && y == shown_y))) return;
    shown_text = p, shown_x = x, shown_y = y;
    shr_lyr_set_visible(composing, p != 0);
    if (p == 0) return;
    char text[3] = {(char)(0xe0 | p >> 12), (char)(0x80 | (p >> 6 & 0x3f)), (char)(0x80 | (p & 0x3f))};
    shr_lyr_set_rect(composing, (shr_rect){x, y, x + 2 * SHR_CELL_WIDTH, y + SHR_CELL_HEIGHT});
    shr_pl_lyr_tilemap_set_cell(composing, 0, 0, text, sizeof text, 2,
                                (shr_text_style){.fg = rgb(colors->background), .bg = rgb(colors->foreground)});
}

void screen_draw(void) {
    GhosttyRenderState render = terminal_update(find_pictures);
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
    draw_pictures();
    if (now.visible != shown_cursor.visible || now.viewport_has_value != shown_cursor.viewport_has_value ||
        now.viewport_x != shown_cursor.viewport_x || now.viewport_y != shown_cursor.viewport_y || now.wide_tail != shown_cursor.wide_tail ||
        now.visual_style != shown_cursor.visual_style)
        draw_cursor(&now, &colors);
    shown_cursor = now;
    draw_composing(&now, &colors);

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
