// SPDX-License-Identifier: GPL-3.0-or-later
// The UI on this computer: an SDL3 window over Shiroko's software driver as the Tab5 draws it before rotating, the
// mouse as touch, made-up values and a stand-in terminal. The layout is read again whenever its file changes. --shot
// saves the screen as BMP after a second and quits.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <SDL3/SDL.h>
#include <shiroko/port_software.h>
#include <shiroko/shiroko.h>

#include "layout.h"
#include "ui.h"

enum { WIDTH = 1280, HEIGHT = 720, COLUMNS = WIDTH / SHR_CELL_WIDTH, ROWS = HEIGHT / SHR_CELL_HEIGHT };
static uint16_t pixels[WIDTH * HEIGHT];
static SDL_Renderer* renderer;
static SDL_Texture* texture;
static const char *layout_path, *fonts;
static shr_lyr* terminal;
static shr_pl_res_bitmap_font* font;
static layout* screen;
static char action[64] = "-";

static shr_status acquire(void* user, shr_surface* out) {
    (void)user;
    *out = (shr_surface){
        .pixels = pixels, .width = WIDTH, .height = HEIGHT, .stride = WIDTH * 2, .byte_length = sizeof pixels, .format = SHR_FORMAT_RGB565};
    return SHR_OK;
}

static shr_status present(void* user, const shr_surface* surface, uint64_t frame) {
    (void)user, (void)frame;
    SDL_UpdateTexture(texture, NULL, surface->pixels, (int)surface->stride);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
    return SHR_OK;
}

static void discard(void* user, const shr_surface* surface) {
    (void)user, (void)surface;
}

static uint64_t now_ns(void* user) {
    (void)user;
    return SDL_GetTicksNS();
}

static shr_status open_package(void* user, const char* package, shr_asset_source* out) {
    (void)user;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", fonts, package);
    return shr_asset_source_file(path, out);
}

static const char* value(const char* name, void* user) {
    (void)user;
    time_t now = time(NULL);
    if (strcmp(name, "host") == 0) return "workbench";
    if (strcmp(name, "status") == 0) return now / 3 % 2 ? "held 2 s" : NULL;
    if (strcmp(name, "network") == 0) {
        static const char* networks[] = {"Wi-Fi MU_GL_XE3000", "LTE", "LTE 미등록 · 다음 시도 73 s"};
        return networks[now / 3 % 3];
    }
    if (strcmp(name, "battery") == 0) return "87%";
    if (strcmp(name, "time") == 0) {
        static char time_text[8];
        strftime(time_text, sizeof time_text, "%H:%M", localtime(&now));
        return time_text;
    }
    return NULL;
}

// Lines of a shell where the layout puts the terminal, and the last action
static void draw_terminal(void) {
    const layout_node* n = layout_find(screen, LAYOUT_TERMINAL);
    shr_lyr_set_visible(terminal, n != NULL && n->area.columns > 0 && n->area.rows > 0);
    if (n == NULL || n->area.columns <= 0 || n->area.rows <= 0) return;
    const layout_area* a = &n->area;
    shr_lyr_set_rect(terminal, (shr_rect){a->column * SHR_CELL_WIDTH, a->row * SHR_CELL_HEIGHT, (a->column + a->columns) * SHR_CELL_WIDTH,
                                          (a->row + a->rows) * SHR_CELL_HEIGHT});
    shr_color black = SHR_RGB(0, 0, 0);
    shr_pl_lyr_tilemap_resize(terminal, font, a->rows, a->columns, &black);
    shr_pl_lyr_tilemap_clear(terminal, 0, 0, a->rows, a->columns, (shr_text_style){0});
    char text[256];
    snprintf(text, sizeof text,
             "tab5@workbench:~$ tmux attach -t tab5-6ca4f771\n"
             "%d x %d cells for the terminal\n"
             "last action: %s\n"
             "tab5@workbench:~$ ",
             a->columns, a->rows, action);
    shr_pl_lyr_tilemap_set_text(terminal, 0, 0, text, strlen(text), (shr_text_style){SHR_RGB(0xd0, 0xd0, 0xd0), 0, 0}, NULL, 0, 0, NULL);
}

static void reload(void) {
    static struct timespec loaded;
    struct stat st;
    if (stat(layout_path, &st) != 0 || (st.st_mtimespec.tv_sec == loaded.tv_sec && st.st_mtimespec.tv_nsec == loaded.tv_nsec)) return;
    loaded = st.st_mtimespec;
    char error[160];
    FILE* f = fopen(layout_path, "rb");
    char* text = calloc(1, (size_t)st.st_size + 1);
    layout* next = NULL;
    if (f != NULL && text != NULL && fread(text, 1, (size_t)st.st_size, f) == (size_t)st.st_size)
        next = layout_parse(text, "main", error, sizeof error);
    else
        snprintf(error, sizeof error, "cannot read");
    if (f != NULL) fclose(f);
    free(text);
    if (next == NULL) {
        fprintf(stderr, "%s: %s\n", layout_path, error);
        return;
    }
    ui_show(next);
    layout_free(screen);
    screen = next;
    draw_terminal();
    printf("%s: %zu nodes\n", layout_path, screen->count);
}

static void touch(const SDL_Event* e) {
    SDL_Event in = *e;
    SDL_ConvertEventToRenderCoordinates(renderer, &in);
    float x = in.type == SDL_EVENT_MOUSE_MOTION ? in.motion.x : in.button.x, y = in.type == SDL_EVENT_MOUSE_MOTION ? in.motion.y : in.button.y;
    ui_touch_phase phase = in.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? UI_TOUCH_DOWN
                           : in.type == SDL_EVENT_MOUSE_BUTTON_UP ? UI_TOUCH_UP
                                                                  : UI_TOUCH_MOVE;
    const char* done = ui_touch(phase, (int32_t)(x / SHR_CELL_WIDTH), (int32_t)(y / SHR_CELL_HEIGHT));
    if (done == NULL) return;
    printf("action %s\n", done);
    snprintf(action, sizeof action, "%s", done);
    draw_terminal();
}

static int check(shr_status status, const char* what) {
    if (status != SHR_OK) fprintf(stderr, "%s: %s\n", what, shr_status_name(status));
    return status != SHR_OK;
}

int main(int argc, char** argv) {
    const char* shot = argc == 5 && strcmp(argv[1], "--shot") == 0 ? argv[2] : NULL;
    if (argc != (shot != NULL ? 5 : 3)) {
        fprintf(stderr, "usage: %s [--shot FILE.bmp] LAYOUT.json FONTS_DIR\n", argv[0]);
        return 2;
    }
    layout_path = argv[argc - 2], fonts = argv[argc - 1];
    SDL_Window* window;
    if (!SDL_Init(SDL_INIT_VIDEO) || !SDL_CreateWindowAndRenderer("tab5-ssh sim", WIDTH, HEIGHT, SDL_WINDOW_RESIZABLE, &window, &renderer) ||
        !SDL_SetRenderLogicalPresentation(renderer, WIDTH, HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX) ||
        (texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT)) == NULL ||
        !SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST)) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    shr_framebuffer_driver driver;
    shr_output output;
    shr_output_init(&output);
    output.acquire = acquire, output.present = present, output.discard = discard;
    output.flags = SHR_OUTPUT_RELEASE_ON_PRESENT | SHR_OUTPUT_PRESERVES_CONTENT;
    shr_context_desc cd;
    shr_context_desc_init(&cd);
    cd.driver = &driver, cd.output = &output, cd.now_ns = now_ns;
    shr_screen_desc sd;
    shr_screen_desc_init(&sd);
    sd.width = WIDTH, sd.height = HEIGHT, sd.clear = SHR_RGB(0, 0, 0);
    shr_pl_res_bitmap_font_desc fd;
    shr_pl_res_bitmap_font_desc_init(&fd);
    fd.open = open_package, fd.locale = "ko";
    shr_context* context;
    if (check(shr_software_driver_create(NULL, 0, 0, 256, &driver), "driver") || check(shr_create(&cd, &context), "context") ||
        check(shr_screen_configure(context, &sd), "screen") || check(shr_pl_res_bitmap_font_create(context, &fd, &font), "font") ||
        check(shr_lyr_create(context, 0, (shr_rect){0, 0, SHR_CELL_WIDTH, SHR_CELL_HEIGHT}, &terminal), "terminal") ||
        check(ui_start(context, font, 6, COLUMNS, ROWS), "ui"))
        return 1;
    for (bool running = true; running;) {
        SDL_Event e;
        for (bool any = SDL_WaitEventTimeout(&e, 30); any; any = SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT)
                running = false;
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                     (e.type == SDL_EVENT_MOUSE_MOTION && (e.motion.state & SDL_BUTTON_LMASK) != 0))
                touch(&e);
        reload();
        ui_draw(value, NULL);
        shr_deadline next;
        shr_event event;
        for (shr_submit(context);;) {
            shr_pump(context);
            while (shr_poll_event(context, &event) == SHR_OK)
                if (event.kind == SHR_EVENT_PRESENT_FAILED || event.kind == SHR_EVENT_RESOURCE_FAILED)
                    fprintf(stderr, "event %d: %s\n", event.kind, shr_status_name(event.status));
            if (shr_next_deadline(context, &next) != SHR_OK || next.kind != SHR_DEADLINE_NOW) break;
        }
        if (shot != NULL && SDL_GetTicks() > 1000) {
            SDL_Surface* s = SDL_CreateSurfaceFrom(WIDTH, HEIGHT, SDL_PIXELFORMAT_RGB565, pixels, WIDTH * 2);
            if (s == NULL || !SDL_SaveBMP(s, shot)) fprintf(stderr, "%s: %s\n", shot, SDL_GetError());
            SDL_DestroySurface(s);
            running = false;
        }
    }
    layout_free(screen);
    SDL_Quit();
    return 0;
}
