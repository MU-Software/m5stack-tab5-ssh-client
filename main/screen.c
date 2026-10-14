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
static shr_lyr *beneath, *grid, *pictures, *cursor, *cursor_text, *composing;
static shr_pl_res_bitmap_font* font;
static int32_t rows_count, columns;
static bool see_through; // the grid lets z < 0 images show under its default background
static GhosttyKittyGraphicsPlacementIterator placements;

// A kitty image as drawn: its source rect scaled to width x height
typedef struct {
    uint32_t id, source_x, source_y, source_width, source_height, width, height;
    uint64_t generation;
    shr_pl_res_image* image;
    bool used;
} picture;

// Its part from `from`, width x height, drawn at `at`
typedef struct {
    shr_pl_res_image* image;
    shr_point at, from;
    int32_t width, height, z;
} placed;

static picture made[16];
static placed shown[64], wanted[64];
static int shown_count, wanted_count;
static GhosttyRenderStateRowIterator rows;
static GhosttyRenderStateRowCells cells;
static GhosttyRenderStateCursor shown_cursor;

// A cell as drawn: its cluster, its style and the lines over it
typedef struct {
    char text[64];
    size_t length;
    uint32_t span;
    shr_text_style style;
    int underline; // GHOSTTY_SGR_UNDERLINE_*
    shr_color underline_color;
    bool strikethrough, overline;
} look;

// The lines of a row, runs of equal neighbours joined
static struct {
    shr_text_line items[3 * 256];
    size_t count;
    size_t last[3];
} lines;

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
    const terminal_size* size = &state->terminal_size;
    rows_count = (int32_t)size->rows, columns = (int32_t)size->columns;
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
        check(shr_lyr_create(context, -1, area, &beneath), "beneath") != SHR_OK || check(shr_lyr_create(context, 0, area, &grid), "grid") != SHR_OK ||
        check(shr_lyr_create(context, 1, area, &pictures), "pictures") != SHR_OK ||
        check(shr_lyr_create(context, 2, area, &cursor), "cursor") != SHR_OK ||
        check(shr_lyr_create(context, 3, (shr_rect){0, 0, 2 * SHR_CELL_WIDTH, SHR_CELL_HEIGHT}, &cursor_text), "cursor text") != SHR_OK ||
        check(shr_pl_lyr_tilemap_resize(cursor_text, font, 1, 2, NULL), "cursor text") != SHR_OK ||
        check(shr_lyr_create(context, 4, (shr_rect){0, 0, 2 * SHR_CELL_WIDTH, SHR_CELL_HEIGHT}, &composing), "composing") != SHR_OK ||
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

static void want(placed e) {
    if (wanted_count == (int)(sizeof wanted / sizeof wanted[0])) return;
    int i = wanted_count++;
    for (; i > 0 && wanted[i - 1].z > e.z; i--)
        wanted[i] = wanted[i - 1];
    wanted[i] = e;
}

// A run of Unicode placeholders: its part of the image fitted into the placement's cells, centred
static void want_run(void* user, const GhosttyKittyGraphicsVirtualRun* run) {
    GhosttyKittyGraphicsImage image = ghostty_kitty_graphics_image(user, run->image_id);
    uint32_t width = 0, height = 0;
    if (image == NULL || ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_WIDTH, &width) != GHOSTTY_SUCCESS ||
        ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_HEIGHT, &height) != GHOSTTY_SUCCESS || width == 0 || height == 0)
        return;
    int64_t box_width = (int64_t)run->grid_cols * SHR_CELL_WIDTH, box_height = (int64_t)run->grid_rows * SHR_CELL_HEIGHT;
    int64_t fit_width = box_width, fit_height = box_height;
    if (width * box_height > height * box_width)
        fit_height = height * box_width / width;
    else
        fit_width = width * box_height / height;
    if (fit_width < 1 || fit_height < 1 || fit_width > 4096 || fit_height > 4096) return;
    int64_t left = (box_width - fit_width) / 2, top = (box_height - fit_height) / 2;
    int64_t x0 = (int64_t)run->col * SHR_CELL_WIDTH, y0 = (int64_t)run->row * SHR_CELL_HEIGHT;
    int64_t x1 = x0 + (int64_t)run->width * SHR_CELL_WIDTH, y1 = y0 + SHR_CELL_HEIGHT;
    int64_t from_x = x0 > left ? x0 : left, from_y = y0 > top ? y0 : top;
    int64_t to_x = x1 < left + fit_width ? x1 : left + fit_width, to_y = y1 < top + fit_height ? y1 : top + fit_height;
    GhosttyKittyGraphicsPlacementRenderInfo r = {
        .source_width = width, .source_height = height, .pixel_width = (uint32_t)fit_width, .pixel_height = (uint32_t)fit_height};
    picture* p;
    if (from_x >= to_x || from_y >= to_y || (p = make_picture(image, run->image_id, &r)) == NULL) return;
    want((placed){p->image,
                  {(int32_t)(run->viewport_col * SHR_CELL_WIDTH + from_x - x0), (int32_t)(run->viewport_row * SHR_CELL_HEIGHT + from_y - y0)},
                  {(int32_t)(from_x - left), (int32_t)(from_y - top)},
                  (int32_t)(to_x - from_x),
                  (int32_t)(to_y - from_y),
                  run->z});
}

// Under the terminal's lock: its kitty images and their places in this frame
static void find_pictures(GhosttyTerminal terminal, GhosttyKittyGraphics graphics) {
    uint64_t generation = 0;
    wanted_count = 0;
    for (size_t i = 0; i < sizeof made / sizeof made[0]; i++)
        made[i].used = false;
    if (ghostty_kitty_graphics_get(graphics, GHOSTTY_KITTY_GRAPHICS_DATA_GENERATION, &generation) != GHOSTTY_SUCCESS || generation == 0 ||
        ghostty_kitty_graphics_get(graphics, GHOSTTY_KITTY_GRAPHICS_DATA_PLACEMENT_ITERATOR, (void*)&placements) != GHOSTTY_SUCCESS)
        return;
    while (ghostty_kitty_graphics_placement_next(placements)) {
        uint32_t id = 0, x = 0, y = 0;
        int32_t z = 0;
        GhosttyKittyGraphicsPlacementRenderInfo r = GHOSTTY_INIT_SIZED(GhosttyKittyGraphicsPlacementRenderInfo);
        ghostty_kitty_graphics_placement_get(placements, GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_IMAGE_ID, &id);
        ghostty_kitty_graphics_placement_get(placements, GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_Z, &z);
        ghostty_kitty_graphics_placement_get(placements, GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_X_OFFSET, &x);
        ghostty_kitty_graphics_placement_get(placements, GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_Y_OFFSET, &y);
        GhosttyKittyGraphicsImage image = ghostty_kitty_graphics_image(graphics, id);
        picture* p;
        if (image == NULL || ghostty_kitty_graphics_placement_render_info(placements, image, terminal, &r) != GHOSTTY_SUCCESS ||
            !r.viewport_visible || r.pixel_width == 0 || r.pixel_height == 0 || (p = make_picture(image, id, &r)) == NULL)
            continue;
        want((placed){p->image,
                      {r.viewport_col * SHR_CELL_WIDTH + (int32_t)x, r.viewport_row * SHR_CELL_HEIGHT + (int32_t)y},
                      {0, 0},
                      (int32_t)p->width,
                      (int32_t)p->height,
                      z});
    }
    ghostty_kitty_graphics_virtual_runs(terminal, want_run, graphics);
}

// z < 0 under the text: the grid's default background turns transparent over the screen's own (the same black)
static void draw_pictures(void) {
    if (wanted_count == shown_count && memcmp(wanted, shown, sizeof wanted[0] * (size_t)wanted_count) == 0) return;
    bool below = wanted_count > 0 && wanted[0].z < 0;
    if (below != see_through) {
        shr_color background = SHR_RGB(0, 0, 0);
        shr_pl_lyr_tilemap_resize(grid, font, rows_count, columns, below ? NULL : &background);
        see_through = below;
    }
    shr_lyr_cmd_begin(beneath);
    shr_lyr_cmd_begin(pictures);
    for (int i = 0; i < wanted_count; i++) {
        const placed* e = &wanted[i];
        shr_rect src = {e->from.x + (e->at.x < 0 ? -e->at.x : 0), e->from.y + (e->at.y < 0 ? -e->at.y : 0), e->from.x + e->width,
                        e->from.y + e->height};
        shr_lyr_cmd_image(e->z < 0 ? beneath : pictures, e->image, src, (shr_point){e->at.x < 0 ? 0 : e->at.x, e->at.y < 0 ? 0 : e->at.y});
    }
    shr_lyr_cmd_commit(beneath);
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

// fg alpha: 255 full, 128 dim, 0 none
static shr_color drawn(GhosttyColorRgb c, const GhosttyStyle* style) {
    return (rgb(c) & 0xffffffu) | (style->invisible ? 0 : style->faint ? 0x80000000u : 0xff000000u);
}

// False for the tail of a wide cell
static bool read_cell(const GhosttyRenderStateColors* colors, look* out) {
    GhosttyCell raw;
    GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
    GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
    GhosttyColorRgb fg = colors->foreground, bg = colors->background;
    char text[sizeof out->text];
    GhosttyBuffer utf8 = {(uint8_t*)text, sizeof text, 0};
    ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw);
    ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide);
    if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) return false;
    ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
    ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg);
    bool painted = ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg) == GHOSTTY_SUCCESS;
    if (ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &utf8) != GHOSTTY_SUCCESS) utf8.len = 0;
    // A kitty image's Unicode placeholder, U+10EEEE: the image is drawn there instead
    if (utf8.len >= 4 && memcmp(text, "\xf4\x8e\xbb\xae", 4) == 0)
        utf8.len = 0, style.underline = GHOSTTY_SGR_UNDERLINE_NONE, style.strikethrough = style.overline = false;
    if (style.inverse) {
        GhosttyColorRgb swap = fg;
        fg = bg, bg = swap, painted = true;
    }
    GhosttyColorRgb under = style.underline_color.tag == GHOSTTY_STYLE_COLOR_RGB       ? style.underline_color.value.rgb
                            : style.underline_color.tag == GHOSTTY_STYLE_COLOR_PALETTE ? colors->palette[style.underline_color.value.palette]
                                                                                       : fg;
    // bg alpha 0 leaves the tilemap's background
    *out = (look){.length = utf8.len,
                  .span = wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : 1,
                  .style = {drawn(fg, &style), painted ? rgb(bg) : 0, (style.bold ? SHR_STYLE_BOLD : 0) | (style.italic ? SHR_STYLE_ITALIC : 0)},
                  .underline = style.underline,
                  .underline_color = drawn(under, &style),
                  .strikethrough = style.strikethrough,
                  .overline = style.overline};
    memcpy(out->text, text, utf8.len);
    return true;
}

static void add_line(int32_t x, uint32_t span, uint8_t kind, uint8_t shape, shr_color color) {
    shr_text_line* last = lines.last[kind] < lines.count ? &lines.items[lines.last[kind]] : NULL;
    if (last != NULL && last->col + last->cols == x && last->shape == shape && last->color == color) {
        last->cols += span;
    } else if (lines.count < sizeof lines.items / sizeof lines.items[0]) {
        lines.last[kind] = lines.count;
        lines.items[lines.count++] = (shr_text_line){.col = (uint16_t)x, .cols = (uint16_t)span, .kind = kind, .shape = shape, .color = color};
    }
}

static void clear_lines(void) {
    lines.count = 0;
    lines.last[0] = lines.last[1] = lines.last[2] = SIZE_MAX;
}

static void add_lines(const look* l, int32_t x, shr_color color) {
    if (l->underline != GHOSTTY_SGR_UNDERLINE_NONE && l->underline <= GHOSTTY_SGR_UNDERLINE_DASHED)
        add_line(x, l->span, SHR_LINE_UNDER, (uint8_t)(l->underline - 1), l->underline_color);
    if (l->strikethrough) add_line(x, l->span, SHR_LINE_STRIKE, SHR_LINE_SINGLE, color);
    if (l->overline) add_line(x, l->span, SHR_LINE_OVER, SHR_LINE_SINGLE, color);
}

static void draw_row(uint16_t y, const GhosttyRenderStateColors* colors) {
    look l;
    clear_lines();
    for (int32_t x = 0; ghostty_render_state_row_cells_next(cells); x++)
        if (read_cell(colors, &l)) {
            shr_pl_lyr_tilemap_set_cell(grid, y, x, l.text, l.length, l.span, l.style);
            add_lines(&l, x, l.style.fg);
        }
    // Every row, as set_cell leaves a row's lines as they were
    shr_pl_lyr_tilemap_set_lines(grid, y, lines.items, lines.count, NULL);
}

// A block shows the cell under it in the cell's background colour
static void draw_cursor(GhosttyRenderState render, const GhosttyRenderStateCursor* c, const GhosttyRenderStateColors* colors) {
    look l = {.span = 1};
    bool on = c->visible && c->viewport_has_value, block = c->visual_style == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK;
    int32_t x = 0, y = 0;
    if (on) {
        x = c->viewport_x - c->wide_tail, y = c->viewport_y;
        ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, (void*)&rows);
        for (int32_t i = 0; i <= y; i++)
            ghostty_render_state_row_iterator_next(rows);
        if (ghostty_render_state_row_get(rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, (void*)&cells) != GHOSTTY_SUCCESS ||
            ghostty_render_state_row_cells_select(cells, (uint16_t)x) != GHOSTTY_SUCCESS || !read_cell(colors, &l))
            l = (look){.span = 1};
    }
    shr_lyr_cmd_begin(cursor);
    if (on) {
        shr_rect r = {x * SHR_CELL_WIDTH, y * SHR_CELL_HEIGHT, (x + (int32_t)l.span) * SHR_CELL_WIDTH, (y + 1) * SHR_CELL_HEIGHT};
        if (c->visual_style == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR) r.x1 = r.x0 + 2;
        if (c->visual_style == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE) r.y0 = r.y1 - 2;
        shr_color color = rgb(colors->cursor_has_value ? colors->cursor : colors->foreground);
        if (c->visual_style == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW) {
            shr_lyr_cmd_fill(cursor, (shr_rect){r.x0, r.y0, r.x1, r.y0 + 2}, color);
            shr_lyr_cmd_fill(cursor, (shr_rect){r.x0, r.y1 - 2, r.x1, r.y1}, color);
            shr_lyr_cmd_fill(cursor, (shr_rect){r.x0, r.y0, r.x0 + 2, r.y1}, color);
            r.x0 = r.x1 - 2;
        }
        shr_lyr_cmd_fill(cursor, r, color);
    }
    shr_lyr_cmd_commit(cursor);
    shr_lyr_set_visible(cursor_text, on && block);
    if (!on || !block) return;
    shr_color text = (l.style.bg >> 24 != 0 ? l.style.bg : rgb(colors->background)) & 0xffffffu;
    l.style.fg = (l.style.fg & 0xff000000u) | text, l.style.bg = 0;
    clear_lines();
    add_lines(&l, 0, l.style.fg);
    for (size_t i = 0; i < lines.count; i++)
        lines.items[i].color = (lines.items[i].color & 0xff000000u) | text;
    shr_lyr_set_rect(cursor_text, (shr_rect){x * SHR_CELL_WIDTH, y * SHR_CELL_HEIGHT, (x + 2) * SHR_CELL_WIDTH, (y + 1) * SHR_CELL_HEIGHT});
    shr_pl_lyr_tilemap_set_cell(cursor_text, 0, 0, l.text, l.length, l.span, l.style);
    shr_pl_lyr_tilemap_set_lines(cursor_text, 0, lines.items, lines.count, NULL);
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
    GhosttyRenderStateDirty dirty = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirty);
    ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_ROW_MOVES, &moves);
    for (int32_t i = 0; i < moves.count; i++)
        shr_pl_lyr_tilemap_scroll(grid, moves.moves[i].top, moves.moves[i].bottom, moves.moves[i].n, (shr_text_style){0});
    ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, (void*)&rows);
    for (uint16_t y; ghostty_render_state_row_iterator_next_dirty(rows, &y);) {
        ghostty_render_state_row_get(rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, (void*)&cells);
        draw_row(y, &colors);
    }
    // The cell under the cursor may have changed with any row
    if (dirty != GHOSTTY_RENDER_STATE_DIRTY_FALSE || now.visible != shown_cursor.visible ||
        now.viewport_has_value != shown_cursor.viewport_has_value || now.viewport_x != shown_cursor.viewport_x ||
        now.viewport_y != shown_cursor.viewport_y || now.wide_tail != shown_cursor.wide_tail || now.visual_style != shown_cursor.visual_style)
        draw_cursor(render, &now, &colors);
    shown_cursor = now;
    ghostty_render_state_clean(render);
    draw_pictures();
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
