// SPDX-License-Identifier: GPL-3.0-or-later
#include "screen.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <bsp/m5stack_tab5.h>
#include <bsp/touch.h>
#include <driver/ppa.h>
#include <esp_cache.h>
#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <shiroko/port_software.h>
#include <shiroko/shiroko.h>

#include "keys.h"
#include "storage.h"
#include "terminal.h"
#include "ui.h"
#include "ui_values.h"

static const char* TAG = "screen";
// NOLINTNEXTLINE(bugprone-reserved-identifier): the linker names it
extern const char _binary_ui_json_start[];

enum { BADGE = 48, COLUMNS = BSP_LCD_V_RES / SHR_CELL_WIDTH, ROWS = BSP_LCD_H_RES / SHR_CELL_HEIGHT }; // cells
static const global_state* state;
static esp_lcd_touch_handle_t touch;
static esp_lcd_panel_handle_t panel;
static shr_surface framebuffer;
static shr_framebuffer_driver software, driver;
static ppa_client_handle_t ppa;
static uint8_t band[BSP_LCD_V_RES * 2 * 16] __attribute__((aligned(CONFIG_CACHE_L2_CACHE_LINE_SIZE)));
static shr_context* context;
static shr_lyr *beneath, *grid, *pictures, *guesses, *cursor, *cursor_text, *composing, *badge;
static shr_pl_res_bitmap_font* font;
// The terminal's cells and where they start
static int32_t rows_count, columns, origin_x, origin_y;
static terminal_size size;
static bool resized;
// The UI's events for screen_event, and what other tasks ask of it or learn from it
static ui_event events[8];
static size_t event_count;
static _Atomic(const char*) open_request;
static atomic_bool open_asked, panel_open, input_seen;
static TickType_t fonts_again;
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

// Keys sent to the host, shown before its echo as mosh does: printable ASCII at the end of the line only. A stretch of
// typing ends with any other key or a wrong guess, and the next one is shown only once the host echoed one of its keys,
// which keeps passwords hidden.
static StreamBufferHandle_t typed;
static atomic_bool typing_over;
static struct {
    char text[256];
    TickType_t at[256];
    int32_t row, col; // of text[0]
    size_t count;
    bool echoed;
} guess;

// What the session waits for, from when
static _Atomic(const char*) waiting;
static atomic_uint waiting_since;

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

// Through internal RAM: the SD card's DMA cannot reach PSRAM pages, and its own bounce buffer for them is a whole
// read's size, which fails while the C6 holds the internal RAM
static shr_status read_package(void* user, uint64_t offset, uint32_t length, void* destination, uint64_t request) {
    static uint8_t chunk[4096] __attribute__((aligned(64)));
    (void)request;
    if (fseek(user, (long)offset, SEEK_SET) != 0) return SHR_E_IO;
    for (uint32_t done = 0, n; done < length; done += n) {
        n = length - done < sizeof chunk ? length - done : sizeof chunk;
        if (fread(chunk, 1, n, user) != n) return SHR_E_IO;
        memcpy((uint8_t*)destination + done, chunk, n);
    }
    return SHR_OK;
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

// Memory used every frame in internal RAM, the rest (font pages, images, layers' cells) in PSRAM: internal RAM keeps
// room for the C6's SDIO buffers
static void* allocate(void* user, size_t size, size_t align, shr_alloc_kind kind) {
    (void)user;
    uint32_t caps = (kind & ~SHR_ALLOC_HOT) == SHR_ALLOC_DMA ? MALLOC_CAP_DMA : (kind & SHR_ALLOC_HOT) != 0 ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM;
    void* p = heap_caps_aligned_alloc(align, size, caps | MALLOC_CAP_8BIT);
    return p != NULL || caps == MALLOC_CAP_DMA ? p : heap_caps_aligned_alloc(align, size, MALLOC_CAP_8BIT);
}

static void release(void* user, void* pointer, size_t size, size_t align, shr_alloc_kind kind) {
    (void)user, (void)size, (void)align, (void)kind;
    heap_caps_free(pointer);
}

static shr_status check(shr_status status, const char* what) {
    if (status != SHR_OK) ESP_LOGE(TAG, "%s: %s", what, shr_status_name(status));
    return status;
}

// The layout from the SD card, else the one built in
static void load_layout(void) {
    char error[160];
    char* text = storage_read_file_text(SD "/.config/tab5-ssh/ui.json");
    bool loaded = text != NULL && ui_load(text, error, sizeof error);
    if (loaded)
        ESP_LOGI(TAG, "layout from the SD card");
    else if (text != NULL)
        ESP_LOGE(TAG, SD "/.config/tab5-ssh/ui.json: %s", error);
    free(text);
    if (!loaded && !ui_load(_binary_ui_json_start, error, sizeof error)) ESP_LOGE(TAG, "built-in layout: %s", error);
}

static layout_area terminal_area(void) {
    layout_area a;
    return ui_terminal(&a) && a.columns >= 2 && a.rows >= 1 ? a : (layout_area){0, 0, COLUMNS, ROWS};
}

// The terminal's layers where the layout puts it
static void place_terminal(void) {
    layout_area a = terminal_area();
    columns = a.columns, rows_count = a.rows, origin_x = a.column * SHR_CELL_WIDTH, origin_y = a.row * SHR_CELL_HEIGHT;
    shr_rect area = {origin_x, origin_y, origin_x + columns * SHR_CELL_WIDTH, origin_y + rows_count * SHR_CELL_HEIGHT};
    shr_color background = SHR_RGB(0, 0, 0);
    shr_lyr* layers[] = {beneath, grid, pictures, cursor};
    for (size_t i = 0; i < sizeof layers / sizeof layers[0]; i++)
        shr_lyr_set_rect(layers[i], area);
    shr_pl_lyr_tilemap_resize(grid, font, rows_count, columns, see_through ? NULL : &background);
    shr_pl_lyr_tilemap_resize(guesses, font, 1, columns, NULL);
    size = (terminal_size){(uint32_t)columns, (uint32_t)rows_count, (uint32_t)(area.x1 - area.x0), (uint32_t)(area.y1 - area.y0)};
    resized = true;
}

esp_err_t screen_start(const global_state* global) {
    state = global;
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
    static const shr_allocator allocator = {NULL, allocate, release, SHR_ALLOC_HOT};
    cd.driver = &driver, cd.output = &output, cd.now_ns = now_ns, cd.allocator = &allocator;
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = BSP_LCD_V_RES, sd.height = BSP_LCD_H_RES, sd.rotation = SHR_ROTATE_90_CCW, sd.clear = SHR_RGB(0, 0, 0);
    shr_surface bands = {
        .pixels = band, .width = BSP_LCD_V_RES, .height = 16, .stride = BSP_LCD_V_RES * 2, .byte_length = sizeof band, .format = SHR_FORMAT_RGB565};
    sd.bands = &bands, sd.band_count = 1, sd.band_align = 16;
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.open = open_package, fd.locale = "ko";
    shr_rect area = {0, 0, COLUMNS * SHR_CELL_WIDTH, ROWS * SHR_CELL_HEIGHT};
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
        check(shr_lyr_create(context, 2, (shr_rect){0, 0, area.x1, SHR_CELL_HEIGHT}, &guesses), "guesses") != SHR_OK ||
        check(shr_pl_lyr_tilemap_resize(guesses, font, 1, COLUMNS, NULL), "guesses") != SHR_OK ||
        check(shr_lyr_create(context, 3, area, &cursor), "cursor") != SHR_OK ||
        check(shr_lyr_create(context, 4, (shr_rect){0, 0, 2 * SHR_CELL_WIDTH, SHR_CELL_HEIGHT}, &cursor_text), "cursor text") != SHR_OK ||
        check(shr_pl_lyr_tilemap_resize(cursor_text, font, 1, 2, NULL), "cursor text") != SHR_OK ||
        check(shr_lyr_create(context, 5, (shr_rect){0, 0, 2 * SHR_CELL_WIDTH, SHR_CELL_HEIGHT}, &composing), "composing") != SHR_OK ||
        check(shr_pl_lyr_tilemap_resize(composing, font, 1, 2, NULL), "composing") != SHR_OK ||
        check(shr_lyr_create(context, 7, (shr_rect){0, 0, BADGE * SHR_CELL_WIDTH, SHR_CELL_HEIGHT}, &badge), "badge") != SHR_OK ||
        check(shr_pl_lyr_tilemap_resize(badge, font, 1, BADGE, NULL), "badge") != SHR_OK || (typed = xStreamBufferCreate(256, 1)) == NULL ||
        check(shr_pl_lyr_tilemap_resize(grid, font, ROWS, COLUMNS, &background), "tilemap") != SHR_OK ||
        check(ui_start(context, font, 6, COLUMNS, ROWS, BSP_LCD_V_RES, ui_values(state)), "ui") != SHR_OK ||
        ghostty_render_state_row_iterator_new(&terminal_allocator, &rows) != GHOSTTY_SUCCESS ||
        ghostty_render_state_row_cells_new(&terminal_allocator, &cells) != GHOSTTY_SUCCESS ||
        ghostty_kitty_graphics_placement_iterator_new(&terminal_allocator, &placements) != GHOSTTY_SUCCESS)
        return ESP_FAIL;
    if (bsp_touch_new(NULL, &touch) != ESP_OK) ESP_LOGE(TAG, "no touch");
    load_layout();
    place_terminal();
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

// Points `cells` at row y of the render state, for ghostty_render_state_row_cells_select
static bool select_row(GhosttyRenderState render, int32_t y) {
    ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, (void*)&rows);
    for (int32_t i = 0; i <= y; i++)
        if (!ghostty_render_state_row_iterator_next(rows)) return false;
    return ghostty_render_state_row_get(rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, (void*)&cells) == GHOSTTY_SUCCESS;
}

// The text of the cell (x, y), or "" when it has none
static void cell_text(GhosttyRenderState render, const GhosttyRenderStateColors* colors, int32_t x, int32_t y, look* out) {
    if (!select_row(render, y) || ghostty_render_state_row_cells_select(cells, (uint16_t)x) != GHOSTTY_SUCCESS || !read_cell(colors, out))
        out->length = 0;
}

static void forget_guesses(bool echoed) {
    guess.count = 0;
    guess.echoed = echoed;
}

static void guess_key(GhosttyRenderState render, const GhosttyRenderStateCursor* c, const GhosttyRenderStateColors* colors, char key) {
    look l;
    if (guess.count == 0) {
        if (!c->visible || !c->viewport_has_value || c->wide_tail || !select_row(render, c->viewport_y)) return;
        for (int32_t x = c->viewport_x; x < columns; x++)
            if (ghostty_render_state_row_cells_select(cells, (uint16_t)x) != GHOSTTY_SUCCESS || (read_cell(colors, &l) && l.length > 0)) return;
        guess.row = c->viewport_y, guess.col = c->viewport_x;
    }
    // Short of the last column, where the host's echo would wrap
    if (guess.col + (int32_t)guess.count >= columns - 1 || guess.count == sizeof guess.text) return;
    guess.text[guess.count] = key;
    guess.at[guess.count++] = xTaskGetTickCount();
}

static void take_keys(GhosttyRenderState render, const GhosttyRenderStateCursor* c, const GhosttyRenderStateColors* colors) {
    uint8_t in[64];
    if (atomic_exchange(&typing_over, false)) forget_guesses(false);
    for (size_t n; (n = xStreamBufferReceive(typed, in, sizeof in, 0)) > 0;)
        for (size_t i = 0; i < n; i++) {
            if (in[i] >= 0x20 && in[i] < 0x7f) {
                guess_key(render, c, colors, (char)in[i]);
            } else if ((in[i] == 0x7f || in[i] == '\b') && guess.count > 0) {
                guess.count--;
            } else {
                forget_guesses(false);
                // The rest of an escape sequence
                if (in[i] == 0x1b) break;
            }
        }
}

// Echoed guesses leave; a wrong one ends the stretch
static void check_guesses(GhosttyRenderState render, const GhosttyRenderStateCursor* c, const GhosttyRenderStateColors* colors) {
    look l;
    if (guess.count > 0 && (!c->viewport_has_value || c->viewport_y != guess.row)) forget_guesses(guess.echoed);
    while (guess.count > 0) {
        cell_text(render, colors, guess.col, guess.row, &l);
        if (l.length == 1 && l.text[0] == guess.text[0]) {
            guess.echoed = true;
            guess.col++, guess.count--;
            memmove(guess.text, guess.text + 1, guess.count);
            memmove(guess.at, guess.at + 1, guess.count * sizeof guess.at[0]);
        } else {
            if (c->viewport_x > guess.col) forget_guesses(false);
            return;
        }
    }
}

// Underlined over the cells after the cursor, which moves to their end; nothing within 50 ms of a key, so a quick echo
// changes nothing
static bool draw_guesses(const GhosttyRenderStateColors* colors) {
    static char drawn_text[256];
    static size_t drawn_count;
    static int32_t drawn_row, drawn_col;
    size_t count = state->echo.enabled && guess.echoed && guess.count > 0 && xTaskGetTickCount() - guess.at[0] >= pdMS_TO_TICKS(state->echo.delay_ms)
                       ? guess.count
                       : 0;
    if (count == drawn_count && (count == 0 || (guess.row == drawn_row && guess.col == drawn_col && memcmp(guess.text, drawn_text, count) == 0)))
        return count > 0;
    drawn_count = count, drawn_row = guess.row, drawn_col = guess.col;
    memcpy(drawn_text, guess.text, count);
    shr_lyr_set_visible(guesses, count > 0);
    if (count == 0) return false;
    shr_text_style style = {rgb(colors->foreground), rgb(colors->background), 0};
    shr_lyr_set_rect(guesses, (shr_rect){origin_x, origin_y + guess.row * SHR_CELL_HEIGHT, origin_x + columns * SHR_CELL_WIDTH,
                                         origin_y + (guess.row + 1) * SHR_CELL_HEIGHT});
    shr_pl_lyr_tilemap_clear(guesses, 0, 0, 1, columns, (shr_text_style){0});
    for (size_t i = 0; i < count; i++)
        shr_pl_lyr_tilemap_set_cell(guesses, 0, guess.col + (int32_t)i, &guess.text[i], 1, 1, style);
    shr_pl_lyr_tilemap_set_lines(
        guesses, 0, &(shr_text_line){.col = (uint16_t)guess.col, .cols = (uint16_t)count, .kind = SHR_LINE_UNDER, .color = style.fg}, 1, NULL);
    return true;
}

bool screen_status(char* out, size_t out_size) {
    const char* what = atomic_load(&waiting);
    TickType_t now = xTaskGetTickCount(), since = (TickType_t)atomic_load(&waiting_since);
    if (what != NULL) snprintf(out, out_size, "%s %lu s", what, (unsigned long)((now - since) / configTICK_RATE_HZ));
    return what != NULL;
}

// Without the top bar, the terminal's top right: what the session waits for and for how long
static void draw_badge(void) {
    static char shown_text[BADGE + 1];
    char text[BADGE + 1] = "", status[BADGE - 2];
    if (!state->bars.top && screen_status(status, sizeof status)) snprintf(text, sizeof text, " %s ", status);
    if (strcmp(text, shown_text) == 0) return;
    memcpy(shown_text, text, sizeof text);
    shr_lyr_set_visible(badge, text[0] != 0);
    if (text[0] == 0) return;
    int32_t n = (int32_t)strlen(text), right = origin_x + columns * SHR_CELL_WIDTH;
    shr_lyr_set_rect(badge, (shr_rect){right - n * SHR_CELL_WIDTH, origin_y, right, origin_y + SHR_CELL_HEIGHT});
    shr_pl_lyr_tilemap_clear(badge, 0, 0, 1, BADGE, (shr_text_style){0});
    shr_pl_lyr_tilemap_set_text(badge, 0, 0, text, (size_t)n, (shr_text_style){SHR_RGB(0, 0, 0), SHR_RGB(0xff, 0xcc, 0), SHR_STYLE_BOLD}, NULL, 0, 0,
                                NULL);
}

void screen_typed(const void* bytes, size_t size) {
    if (bytes == NULL)
        atomic_store(&typing_over, true);
    else if (typed != NULL)
        xStreamBufferSend(typed, bytes, size, 0);
}

void screen_waiting(const char* what, TickType_t since) {
    if (what != NULL && what == atomic_load(&waiting)) return;
    atomic_store(&waiting_since, since);
    atomic_store(&waiting, what);
}

// A block shows the cell under it in the cell's background colour
static void draw_cursor(GhosttyRenderState render, const GhosttyRenderStateCursor* c, const GhosttyRenderStateColors* colors) {
    look l = {.span = 1};
    bool on = c->visible && c->viewport_has_value, block = c->visual_style == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK;
    int32_t x = 0, y = 0;
    if (on) {
        x = c->viewport_x - c->wide_tail, y = c->viewport_y;
        if (!select_row(render, y) || ghostty_render_state_row_cells_select(cells, (uint16_t)x) != GHOSTTY_SUCCESS || !read_cell(colors, &l))
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
    shr_lyr_set_rect(cursor_text, (shr_rect){origin_x + x * SHR_CELL_WIDTH, origin_y + y * SHR_CELL_HEIGHT, origin_x + (x + 2) * SHR_CELL_WIDTH,
                                             origin_y + (y + 1) * SHR_CELL_HEIGHT});
    shr_pl_lyr_tilemap_set_cell(cursor_text, 0, 0, l.text, l.length, l.span, l.style);
    shr_pl_lyr_tilemap_set_lines(cursor_text, 0, lines.items, lines.count, NULL);
}

// The Korean syllable being typed, over the cursor's cell
static void draw_composing(const GhosttyRenderStateCursor* c, const GhosttyRenderStateColors* colors) {
    static uint32_t shown_text;
    static int32_t shown_x, shown_y;
    uint32_t p = keys_preedit();
    int32_t x = origin_x + c->viewport_x * SHR_CELL_WIDTH, y = origin_y + c->viewport_y * SHR_CELL_HEIGHT;
    if (p == shown_text && (p == 0 || (x == shown_x && y == shown_y))) return;
    shown_text = p, shown_x = x, shown_y = y;
    shr_lyr_set_visible(composing, p != 0);
    if (p == 0) return;
    char text[3] = {(char)(0xe0 | p >> 12), (char)(0x80 | (p >> 6 & 0x3f)), (char)(0x80 | (p & 0x3f))};
    shr_lyr_set_rect(composing, (shr_rect){x, y, x + 2 * SHR_CELL_WIDTH, y + SHR_CELL_HEIGHT});
    shr_pl_lyr_tilemap_set_cell(composing, 0, 0, text, sizeof text, 2,
                                (shr_text_style){.fg = rgb(colors->background), .bg = rgb(colors->foreground)});
}

static void push(ui_event e) {
    if (e.kind != UI_NONE && event_count < sizeof events / sizeof events[0]) events[event_count++] = e;
}

// Panel cells: x runs up the portrait panel's height, y along its width
static void poll_touch(void) {
    static bool down;
    static int32_t column, row;
    esp_lcd_touch_point_data_t p = {0};
    uint8_t n = 0;
    if (touch == NULL || esp_lcd_touch_read_data(touch) != ESP_OK || esp_lcd_touch_get_data(touch, &p, &n, 1) != ESP_OK) n = 0;
    if (n == 0 && !down) return;
    ui_touch_phase phase = n == 0 ? UI_TOUCH_UP : down ? UI_TOUCH_MOVE : UI_TOUCH_DOWN;
    if (n > 0) {
        int32_t c = (BSP_LCD_V_RES - 1 - p.y) / SHR_CELL_WIDTH, r = p.x / SHR_CELL_HEIGHT;
        if (phase == UI_TOUCH_MOVE && c == column && r == row) return;
        column = c, row = r;
    }
    if (phase == UI_TOUCH_DOWN) {
        atomic_store(&input_seen, true);
        ESP_LOGD(TAG, "touch %u,%u: cell %ld,%ld", p.x, p.y, (long)column, (long)row);
    }
    down = n > 0;
    push(ui_touch(phase, column, row));
}

// Keys go to an open panel; once it closes the rest go to the terminal
static void take_ui_keys(void) {
    static const struct {
        uint8_t usage;
        ui_key_code key;
    } named[] = {{0x52, UI_KEY_UP},    {0x51, UI_KEY_DOWN},  {0x50, UI_KEY_LEFT},   {0x4f, UI_KEY_RIGHT},    {0x2b, UI_KEY_TAB},
                 {0x28, UI_KEY_ENTER}, {0x58, UI_KEY_ENTER}, {0x29, UI_KEY_ESCAPE}, {0x2a, UI_KEY_BACKSPACE}};
    key_event e;
    char c;
    keys_divert(ui_panel() != NULL);
    while (ui_panel() != NULL && keys_take(&e, &c)) {
        size_t i = 0;
        while (i < sizeof named / sizeof named[0] && named[i].usage != e.usage)
            i++;
        atomic_store(&input_seen, true);
        if (i < sizeof named / sizeof named[0])
            push(ui_key(named[i].key == UI_KEY_TAB && (e.modifiers & 0x22) != 0 ? UI_KEY_BACKTAB : named[i].key, ""));
        else if (c >= 0x20 && c < 0x7f)
            push(ui_key(UI_KEY_TEXT, (char[]){c, 0}));
    }
    keys_divert(ui_panel() != NULL);
}

// The UI over the terminal, which moves and resizes with the layout
static void draw_ui(void) {
    if (atomic_exchange(&open_asked, false)) ui_open(atomic_load(&open_request));
    poll_touch();
    take_ui_keys();
    ui_draw();
    atomic_store(&panel_open, ui_panel() != NULL);
    layout_area a = terminal_area();
    if (a.columns == columns && a.rows == rows_count && a.column * SHR_CELL_WIDTH == origin_x && a.row * SHR_CELL_HEIGHT == origin_y) return;
    place_terminal();
    terminal_resize(&size);
    forget_guesses(false);
}

bool screen_event(ui_event* out) {
    if (event_count == 0) return false;
    *out = events[0];
    memmove(events, events + 1, --event_count * sizeof events[0]);
    return true;
}

bool screen_resized(terminal_size* out) {
    if (!resized) return false;
    *out = size, resized = false;
    return true;
}

void screen_open(const char* panel) {
    atomic_store(&open_request, panel);
    atomic_store(&open_asked, true);
}

bool screen_panel_open(void) {
    return atomic_load(&panel_open) || atomic_load(&open_asked);
}

bool screen_input(void) {
    return atomic_exchange(&input_seen, false);
}

void screen_draw(void) {
    // A font that failed to load is tried again a second later, not only after Shiroko's three quick retries
    if (fonts_again != 0 && (int32_t)(xTaskGetTickCount() - fonts_again) >= 0) {
        fonts_again = 0;
        shr_asset_ready(context);
    }
    draw_ui();
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
    take_keys(render, &now, &colors);
    check_guesses(render, &now, &colors);
    if (draw_guesses(&colors)) now.viewport_x = (uint16_t)(guess.col + (int32_t)guess.count), now.wide_tail = false;
    // The cell under the cursor may have changed with any row
    if (dirty != GHOSTTY_RENDER_STATE_DIRTY_FALSE || now.visible != shown_cursor.visible ||
        now.viewport_has_value != shown_cursor.viewport_has_value || now.viewport_x != shown_cursor.viewport_x ||
        now.viewport_y != shown_cursor.viewport_y || now.wide_tail != shown_cursor.wide_tail || now.visual_style != shown_cursor.visual_style)
        draw_cursor(render, &now, &colors);
    shown_cursor = now;
    ghostty_render_state_clean(render);
    draw_pictures();
    draw_composing(&now, &colors);
    draw_badge();

    shr_deadline next;
    shr_event event;
    for (shr_submit(context);;) {
        shr_pump(context);
        while (shr_poll_event(context, &event) == SHR_OK)
            if (event.kind == SHR_EVENT_PRESENT_FAILED || event.kind == SHR_EVENT_RESOURCE_FAILED) {
                ESP_LOGW(TAG, "event %d: %s", event.kind, shr_status_name(event.status));
                if (event.kind == SHR_EVENT_RESOURCE_FAILED && fonts_again == 0) fonts_again = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
            }
        if (shr_next_deadline(context, &next) != SHR_OK || next.kind != SHR_DEADLINE_NOW) return;
    }
}
