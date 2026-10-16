// SPDX-License-Identifier: GPL-3.0-or-later
// The UI on this computer: an SDL3 window over Shiroko's software driver as the Tab5 draws it before rotating, the
// mouse as touch, made-up values and a stand-in terminal. The layout is read again whenever its file changes.
// --input runs touches and keys first ("tap:COLUMN,ROW", "key:tab|backtab|up|down|left|right|enter|escape|backspace",
// "text:UTF-8"), --shot saves the screen as BMP after a second and quits.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <SDL3/SDL.h>
#include <shiroko/port_software.h>
#include <shiroko/shiroko.h>

#include "layout.h"
#include "settings.h"
#include "ui.h"

enum { WIDTH = 1280, HEIGHT = 720, COLUMNS = WIDTH / SHR_CELL_WIDTH, ROWS = HEIGHT / SHR_CELL_HEIGHT };
static uint16_t pixels[WIDTH * HEIGHT];
static SDL_Renderer* renderer;
static SDL_Texture* texture;
static const char *layout_path, *fonts;
static shr_lyr* terminal;
static shr_pl_res_bitmap_font* font;
static char action[96] = "-";
static global_state state = {
    .log = {.enabled = true, .file_kb = 1024, .keep_mb = 32, .keep_days = 14},
    .timezone = "KST-9",
    .echo = {true, 50},
    .bars = {true, true},
};
static ssh_host hosts[] = {
    {"workbench", "workbench.lan", "tab5", "", 22, NULL},
    {"nas", "nas.lan", "admin", "", 22, NULL},
    {"cloud", "ssh.example.org", "me", "", 2222, NULL},
};
static const char* networks[] = {"MU_GL_XE3000", "YGYguest", "iPhone 15"};

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

static const char* text(void* user, const char* name) {
    (void)user;
    time_t now = time(NULL);
    if (strcmp(name, "host") == 0) return state.current_host->alias;
    if (strcmp(name, "status") == 0) return now / 3 % 2 ? "held 2 s" : NULL;
    if (strcmp(name, "network") == 0) return state.wifi_off ? "LTE" : "Wi-Fi MU_GL_XE3000";
    if (strcmp(name, "path") == 0) return state.wifi_off ? "LTE (ppp0 10.170.3.41)" : "Wi-Fi (10.1.0.23)";
    if (strcmp(name, "wifi.state") == 0) return state.wifi_off ? "꺼짐 (저장)" : "MU_GL_XE3000 연결됨";
    if (strcmp(name, "wifi.detail") == 0) return state.wifi_off ? "" : "10.1.0.23 · -58 dBm · 5 GHz";
    if (strcmp(name, "lte.state") == 0) {
        static const char* const lte[] = {"연결됨", "미등록 · 다음 시도 73 s", "다이얼 중"};
        return lte[now / 3 % 3];
    }
    if (strcmp(name, "lte.detail") == 0) return "SKT · CAT-M · -97 dBm";
    if (strcmp(name, "battery") == 0) return "87%";
    if (strcmp(name, "time") == 0) {
        static char time_text[8];
        strftime(time_text, sizeof time_text, "%H:%M", localtime(&now));
        return time_text;
    }
    return NULL;
}

static size_t count(void* user, const char* name) {
    (void)user;
    return strcmp(name, "hosts") == 0 ? sizeof hosts / sizeof hosts[0] : strcmp(name, "wifi") == 0 ? sizeof networks / sizeof networks[0] : 0;
}

static const char* item(void* user, const char* name, size_t index, const char* field) {
    (void)user;
    if (strcmp(name, "wifi") == 0) return strcmp(field, "ssid") == 0 ? networks[index] : NULL;
    const ssh_host* h = &hosts[index];
    if (strcmp(field, "alias") == 0) return h->alias;
    if (strcmp(field, "hostname") == 0) return h->hostname;
    if (strcmp(field, "user") == 0) return h->user;
    if (strcmp(field, "current") == 0) return h == state.current_host ? "← 지금" : NULL;
    if (strcmp(field, "port") == 0) {
        static char port[8];
        snprintf(port, sizeof port, "%u", h->port);
        return port;
    }
    return NULL;
}

static size_t chosen(void* user, const char* name) {
    (void)user;
    return strcmp(name, "hosts") == 0 ? (size_t)(state.current_host - hosts) : 0;
}

static void get(void* user, const setting* s, setting_value* out) {
    (void)user;
    settings_get(&state, s, out);
}

static const ui_source source = {text, count, item, chosen, get, NULL};

// Lines of a shell where the layout puts the terminal, and the last event
static void draw_terminal(void) {
    layout_area a;
    bool on = ui_terminal(&a) && a.columns > 0 && a.rows > 0;
    shr_lyr_set_visible(terminal, on);
    if (!on) return;
    shr_lyr_set_rect(terminal, (shr_rect){a.column * SHR_CELL_WIDTH, a.row * SHR_CELL_HEIGHT, (a.column + a.columns) * SHR_CELL_WIDTH,
                                          (a.row + a.rows) * SHR_CELL_HEIGHT});
    shr_color black = SHR_RGB(0, 0, 0);
    shr_pl_lyr_tilemap_resize(terminal, font, a.rows, a.columns, &black);
    shr_pl_lyr_tilemap_clear(terminal, 0, 0, a.rows, a.columns, (shr_text_style){0});
    char lines[256];
    snprintf(lines, sizeof lines,
             "tab5@%s:~$ tmux attach -t tab5-6ca4f771\n"
             "%d x %d cells for the terminal\n"
             "last: %s\n"
             "tab5@%s:~$ ",
             state.current_host->alias, a.columns, a.rows, action, state.current_host->alias);
    shr_pl_lyr_tilemap_set_text(terminal, 0, 0, lines, strlen(lines), (shr_text_style){SHR_RGB(0xd0, 0xd0, 0xd0), 0, 0}, NULL, 0, 0, NULL);
}

// What main.c is to do with an event
static void handle(const ui_event* e) {
    if (e->kind == UI_SET) {
        char value[80];
        bool ok = settings_set(&state, e->setting, &e->value);
        settings_text(e->setting, &e->value, value, sizeof value);
        snprintf(action, sizeof action, "set %s %s%s", e->name, value, ok ? "" : " (refused)");
    } else if (e->kind == UI_ACTION) {
        snprintf(action, sizeof action, "%s %zu", e->name, e->index);
        if (strcmp(e->name, "connect") == 0) {
            state.current_host = &hosts[e->index];
            ui_open(NULL);
        }
        if (strcmp(e->name, "wifi on") == 0 || strcmp(e->name, "wifi off") == 0) state.wifi_off = e->name[6] == 'f';
    } else {
        return;
    }
    printf("%s\n", action);
    draw_terminal();
}

static void reload(void) {
    static struct timespec loaded;
    struct stat st;
    if (stat(layout_path, &st) != 0 || (st.st_mtimespec.tv_sec == loaded.tv_sec && st.st_mtimespec.tv_nsec == loaded.tv_nsec)) return;
    loaded = st.st_mtimespec;
    char error[160] = "cannot read";
    FILE* f = fopen(layout_path, "rb");
    char* json = calloc(1, (size_t)st.st_size + 1);
    bool ok = f != NULL && json != NULL && fread(json, 1, (size_t)st.st_size, f) == (size_t)st.st_size && ui_load(json, error, sizeof error);
    if (f != NULL) fclose(f);
    free(json);
    if (!ok) {
        fprintf(stderr, "%s: %s\n", layout_path, error);
        return;
    }
    printf("%s: loaded\n", layout_path);
}

static void touch(const SDL_Event* e) {
    SDL_Event in = *e;
    SDL_ConvertEventToRenderCoordinates(renderer, &in);
    float x = in.type == SDL_EVENT_MOUSE_MOTION ? in.motion.x : in.button.x, y = in.type == SDL_EVENT_MOUSE_MOTION ? in.motion.y : in.button.y;
    ui_touch_phase phase = in.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? UI_TOUCH_DOWN
                           : in.type == SDL_EVENT_MOUSE_BUTTON_UP ? UI_TOUCH_UP
                                                                  : UI_TOUCH_MOVE;
    ui_event done = ui_touch(phase, (int32_t)(x / SHR_CELL_WIDTH), (int32_t)(y / SHR_CELL_HEIGHT));
    handle(&done);
}

static const struct {
    const char* name;
    SDL_Keycode sdl;
    ui_key_code key;
} keys[] = {{"up", SDLK_UP, UI_KEY_UP},           {"down", SDLK_DOWN, UI_KEY_DOWN},       {"left", SDLK_LEFT, UI_KEY_LEFT},
            {"right", SDLK_RIGHT, UI_KEY_RIGHT},  {"tab", SDLK_TAB, UI_KEY_TAB},          {"backtab", SDLK_UNKNOWN, UI_KEY_BACKTAB},
            {"enter", SDLK_RETURN, UI_KEY_ENTER}, {"escape", SDLK_ESCAPE, UI_KEY_ESCAPE}, {"backspace", SDLK_BACKSPACE, UI_KEY_BACKSPACE}};

static void key(const SDL_KeyboardEvent* k) {
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (keys[i].sdl == k->key && k->key != SDLK_UNKNOWN) {
            ui_event e = ui_key(keys[i].key == UI_KEY_TAB && (k->mod & SDL_KMOD_SHIFT) ? UI_KEY_BACKTAB : keys[i].key, "");
            handle(&e);
        }
}

static void run_input(const char* script) {
    char copy[1024];
    snprintf(copy, sizeof copy, "%s", script);
    for (char *save, *step = strtok_r(copy, " ", &save); step != NULL; step = strtok_r(NULL, " ", &save)) {
        int column, row;
        ui_event e = {.kind = UI_NONE};
        if (sscanf(step, "tap:%d,%d", &column, &row) == 2) {
            ui_touch(UI_TOUCH_DOWN, column, row);
            e = ui_touch(UI_TOUCH_UP, column, row);
        } else if (strncmp(step, "text:", 5) == 0) {
            e = ui_key(UI_KEY_TEXT, step + 5);
        } else {
            size_t i = 0;
            while (i < sizeof keys / sizeof keys[0] && (strncmp(step, "key:", 4) != 0 || strcmp(step + 4, keys[i].name) != 0))
                i++;
            if (i == sizeof keys / sizeof keys[0]) {
                fprintf(stderr, "input %s: not tap:C,R, key:NAME or text:TEXT\n", step);
                continue;
            }
            e = ui_key(keys[i].key, "");
        }
        handle(&e);
        ui_draw();
    }
}

static int check(shr_status status, const char* what) {
    if (status != SHR_OK) fprintf(stderr, "%s: %s\n", what, shr_status_name(status));
    return status != SHR_OK;
}

int main(int argc, char** argv) {
    const char *shot = NULL, *input = NULL;
    int a = 1;
    for (; a + 1 < argc && (strcmp(argv[a], "--shot") == 0 || strcmp(argv[a], "--input") == 0); a += 2)
        *(argv[a][2] == 's' ? &shot : &input) = argv[a + 1];
    if (argc - a != 2) {
        fprintf(stderr, "usage: %s [--input STEPS] [--shot FILE.bmp] LAYOUT.json FONTS_DIR\n", argv[0]);
        return 2;
    }
    layout_path = argv[a], fonts = argv[a + 1];
    for (size_t i = 0; i + 1 < sizeof hosts / sizeof hosts[0]; i++)
        hosts[i].next = &hosts[i + 1];
    state.hosts = hosts, state.current_host = hosts;
    SDL_Window* window;
    if (!SDL_Init(SDL_INIT_VIDEO) || !SDL_CreateWindowAndRenderer("tab5-ssh sim", WIDTH, HEIGHT, SDL_WINDOW_RESIZABLE, &window, &renderer) ||
        !SDL_SetRenderLogicalPresentation(renderer, WIDTH, HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX) ||
        (texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT)) == NULL ||
        !SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST) || !SDL_StartTextInput(window)) {
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
        check(ui_start(context, font, 6, COLUMNS, ROWS, WIDTH, &source), "ui"))
        return 1;
    reload();
    // As at boot with more than one host
    ui_open("hosts");
    if (input != NULL) run_input(input);
    layout_area shown_terminal = {0};
    for (bool running = true; running;) {
        SDL_Event e;
        for (bool any = SDL_WaitEventTimeout(&e, 30); any; any = SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                       (e.type == SDL_EVENT_MOUSE_MOTION && (e.motion.state & SDL_BUTTON_LMASK) != 0)) {
                touch(&e);
            } else if (e.type == SDL_EVENT_KEY_DOWN) {
                key(&e.key);
            } else if (e.type == SDL_EVENT_TEXT_INPUT && ui_panel() != NULL) {
                ui_event done = ui_key(UI_KEY_TEXT, e.text.text);
                handle(&done);
            }
        reload();
        ui_draw();
        layout_area now;
        if (ui_terminal(&now) && memcmp(&now, &shown_terminal, sizeof now) != 0) shown_terminal = now, draw_terminal();
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
    SDL_Quit();
    return 0;
}
