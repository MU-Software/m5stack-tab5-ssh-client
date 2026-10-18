// SPDX-License-Identifier: GPL-3.0-or-later
// The UI on this computer: an SDL3 window over Shiroko's software driver as the Tab5 draws it before rotating, the
// mouse as touch, made-up values and a stand-in terminal. The layout is read again whenever its file changes.
// --input runs touches and keys first ("tap:COLUMN,ROW", "key:tab|backtab|up|down|left|right|enter|escape|backspace",
// "text:UTF-8", "dialog:alert|confirm|prompt"), --shot saves the screen as BMP after a second and quits.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <SDL3/SDL.h>
#include <shiroko/port_software.h>
#include <shiroko/shiroko.h>

#include "layout.h"
#include "modem.h"
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
    .wifi_enabled = true,
    .lte_enabled = true,
};
static ssh_host hosts[] = {
    {"workbench", "workbench.lan", "tab5", "", 22, NULL},
    {"nas", "nas.lan", "admin", "", 22, NULL},
    {"cloud", "ssh.example.org", "me", "", 2222, NULL},
};
static char networks[16][33] = {"MU_GL_XE3000", "YGYguest", "iPhone 15"};
static size_t network_count = 3;
// As the research module answered; read_at only needs to be non-zero
static const modem_snapshot modem = {
    .values =
        {
            [MODEM_QUALCOMM_URC] = "1",
            [MODEM_BATTERY_CHECK] = "1",
            [MODEM_EDRX] = "\"0001\"",
            [MODEM_EDRX_NETWORK] = "0",
            [MODEM_PSM] = "0,,,\"01100000\",\"00000000\"",
            [MODEM_MODE] = "2",
            [MODEM_CATEGORY] = "1",
            [MODEM_BANDS] = "1,2,3,4,5,8,12,13,14,18,19,20,25,26,27,28,66,85",
            [MODEM_REVISION] = "1951B17SIM7080",
            [MODEM_REJECTS] = "0",
            [MODEM_BACKOFF] = "1,00000000000000000000000000000000",
            [MODEM_REGISTRATION] = "0,1",
            [MODEM_SYSTEM] = "LTE CAT-M1,Online,450-06,0x420F,14841609,82,EUTRAN-BAND5,2506,0,0,-7,-102,-75,13",
            [MODEM_BEARER] = "1",
            [MODEM_SIGNAL] = "18,99",
            [MODEM_OPERATOR] = "0,0,\"LG U+\",7",
            [MODEM_SIM] = "READY",
            [MODEM_ICCID] = "898206…1234",
            [MODEM_NUMBER] = "\"\",\"01012345678\",129",
            [MODEM_CONTEXT] = "\"IP\",\"internet.lguplus.co.kr\",\"0.0.0.0\",0,0,0,0",
            [MODEM_ADDRESS] = "\"10.53.105.62\"",
        },
    .read_at = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
};
static const struct {
    const char* ssid;
    int rssi;
    bool open;
} visible[] = {{"MU_GL_XE3000", -48, false}, {"KT_GiGA_5G_3F21", -61, false}, {"YGYguest", -70, true}, {"DIRECT-7B-HP M28w", -77, false}};
static const char* const session_states[] = {"끊김", "연결 중", "연결됨", "다시 연결 중", "입력 기다림", "삭제 중"};

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

// visible[]'s index of the index-th network not saved, -1 past the last
static int unsaved(size_t index) {
    for (size_t i = 0; i < sizeof visible / sizeof visible[0]; i++) {
        bool saved = false;
        for (size_t k = 0; k < network_count; k++)
            saved |= strcmp(networks[k], visible[i].ssid) == 0;
        if (!saved && index-- == 0) return (int)i;
    }
    return -1;
}

static const char* wifi_text(const char* name) {
    if (strcmp(name, "path") == 0) return state.wifi_enabled ? "●" : NULL;
    if (strcmp(name, "toggle") == 0) return state.wifi_enabled ? "끄기" : "켜기";
    if (strcmp(name, "count") == 0) {
        static char saved[8];
        return snprintf(saved, sizeof saved, "%zu/16", network_count), saved;
    }
    return NULL;
}

static const char* text(void* user, const char* name) {
    (void)user;
    time_t now = time(NULL);
    static char tab[64];
    int number, end = 0;
    if (sscanf(name, "session.%d%n", &number, &end) == 1 && name[end] == 0) {
        for (size_t i = 0; i < state.session_count; i++) {
            const session_info* s = &state.sessions[i];
            if (s->number != number) continue;
            snprintf(tab, sizeof tab, "%s %d %s%s", s->shown ? "●" : "○", number, s->alias,
                     s->alert ? " !" : (const char* const[]){" ×", " …", "", " …", " ?", " ×"}[s->status]);
            return tab;
        }
        return NULL;
    }
    if (strcmp(name, "sessions.none") == 0) return state.session_count == 0 ? "1" : NULL;
    if (strcmp(name, "sessions.any") == 0) return state.session_count > 0 ? "1" : NULL;
    if (strcmp(name, "hosts.room") == 0) return state.session_count < MAX_SESSIONS ? "1" : NULL;
    if (strcmp(name, "sessions") == 0) {
        snprintf(tab, sizeof tab, "%zu/%d", state.session_count, MAX_SESSIONS);
        return tab;
    }
    if (strcmp(name, "status") == 0) return now / 3 % 2 ? "held 2 s" : NULL;
    if (strcmp(name, "network") == 0) return !state.wifi_enabled ? "LTE" : "Wi-Fi MU_GL_XE3000";
    if (strcmp(name, "wifi.state") == 0) return !state.wifi_enabled ? "꺼짐" : "MU_GL_XE3000 연결됨";
    if (strcmp(name, "wifi.detail") == 0) return !state.wifi_enabled ? "" : "10.1.0.23 · -58 dBm";
    if (strcmp(name, "wifi.scan") == 0) return now / 3 % 2 ? "찾는 중…" : NULL;
    if (strcmp(name, "lte.path") == 0) return !state.wifi_enabled && state.lte_enabled ? "●" : NULL;
    if (strcmp(name, "lte.toggle") == 0) return state.lte_enabled ? "끄기" : "켜기";
    if (strcmp(name, "lte.state") == 0) {
        static const char* const lte[] = {"연결됨", "미등록 · 다음 시도 73 s", "다이얼 중"};
        return state.lte_enabled ? lte[now / 3 % 3] : "꺼짐";
    }
    if (strcmp(name, "lte.detail") == 0) return "SKT · CAT-M · -97 dBm";
    if (strncmp(name, "wifi.", 5) == 0) return wifi_text(name + 5);
    if (strcmp(name, "battery") == 0) {
        snprintf(tab, sizeof tab, "%s%s", now / 3 % 2 ? "⚡" : "", state.battery == 1 ? "87%" : "8.21 V");
        return tab;
    }
    if (strcmp(name, "modem.read") == 0) return "PPP 중: 다이얼 전에 읽은 값 · 다시 읽으려면 LTE 끄기";
    if (strncmp(name, "modem.", 6) == 0 && strstr(name, ".age") != NULL) return "12분 전";
    if (strncmp(name, "modem.", 6) == 0) {
        static char modem_text_buffer[96];
        return modem_text(&modem, name + 6, modem_text_buffer, sizeof modem_text_buffer) != MODEM_ITEMS ? modem_text_buffer : NULL;
    }
    if (strcmp(name, "time") == 0) {
        static char time_text[8];
        strftime(time_text, sizeof time_text, "%H:%M", localtime(&now));
        return time_text;
    }
    return NULL;
}

static size_t count(void* user, const char* name) {
    (void)user;
    size_t n = 0;
    while (strcmp(name, "visible") == 0 && unsaved(n) >= 0)
        n++;
    if (strcmp(name, "visible") == 0) return n;
    if (strcmp(name, "sessions") == 0) return state.session_count;
    return strcmp(name, "hosts") == 0 ? sizeof hosts / sizeof hosts[0] : strcmp(name, "wifi") == 0 ? network_count : 0;
}

static const char* item(void* user, const char* name, size_t index, const char* field) {
    (void)user;
    static char number[16];
    if (strcmp(name, "wifi") == 0 && strcmp(field, "order") == 0) return snprintf(number, sizeof number, "%zu", index + 1), number;
    if (strcmp(name, "wifi") == 0 && strcmp(field, "joined") == 0) return index == 0 && state.wifi_enabled ? "연결됨" : NULL;
    if (strcmp(name, "wifi") == 0 && strcmp(field, "join") == 0) return index == 0 && state.wifi_enabled ? NULL : "1";
    if (strcmp(name, "wifi") == 0 && strcmp(field, "up") == 0) return index > 0 ? "1" : NULL;
    if (strcmp(name, "wifi") == 0) return strcmp(field, "ssid") == 0 ? networks[index] : NULL;
    const session_info* s = strcmp(name, "sessions") == 0 ? &state.sessions[index] : NULL;
    if (s != NULL && strcmp(field, "number") == 0) return snprintf(number, sizeof number, "%d", s->number), number;
    if (s != NULL && strcmp(field, "alias") == 0) return s->alias;
    if (s != NULL && strcmp(field, "status") == 0) return session_states[s->status];
    if (s != NULL && strcmp(field, "toggle") == 0) return s->status == SESSION_DISCONNECTED ? "연결" : "끊기";
    if (s != NULL && strcmp(field, "idle") == 0) return s->status != SESSION_DELETING && !s->creating ? "1" : NULL;
    if (s != NULL) return NULL;
    if (strcmp(name, "visible") == 0) {
        int i = unsaved(index);
        if (i < 0) return NULL;
        if (strcmp(field, "ssid") == 0) return visible[i].ssid;
        if (strcmp(field, "signal") == 0) return snprintf(number, sizeof number, "%d dBm", visible[i].rssi), number;
        if (strcmp(field, "lock") == 0) return visible[i].open ? "열림" : NULL;
        return NULL;
    }
    const ssh_host* h = &hosts[index];
    if (strcmp(field, "alias") == 0) return h->alias;
    if (strcmp(field, "hostname") == 0) return h->hostname;
    if (strcmp(field, "user") == 0) return h->user;
    if (strcmp(field, "port") == 0 && h->port != 22) {
        static char port[8];
        snprintf(port, sizeof port, ":%u", h->port);
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

// The action a dialog's 확인 does
static char asked[48];
#define PASSWORD "비밀번호를 입력하세요.\n비밀번호가 없는 개방형 네트워크는 비운 채로 저장해주세요."

// As main.c's network screen's Wi-Fi, without joining: a pick saved and a delete done once confirmed
static void wifi_screen(const ui_event* e) {
    size_t i = e->index;
    if (strcmp(e->name, "wifi add") == 0)
        snprintf(asked, sizeof asked, "wifi type"),
            ui_dialog_list("사용 가능한 Wi-Fi", "visible", "  {ssid:34}{signal:10}{lock}", "wifi.scan", "직접 입력");
    if (strcmp(e->name, "wifi type") == 0) snprintf(asked, sizeof asked, "wifi typed"), ui_dialog(UI_PROMPT, "직접 입력", "SSID", "다음");
    if (strcmp(e->name, "wifi typed") == 0) ui_dialog(UI_PASSWORD, "(SSID)", PASSWORD, "연결");
    if (strcmp(e->name, "wifi pick") == 0 && unsaved(i) >= 0) {
        i = (size_t)unsaved(i);
        snprintf(asked, sizeof asked, "wifi saved %zu", i);
        if (visible[i].open)
            ui_dialog(UI_CONFIRM, visible[i].ssid, "열린 망 · 연결되면 저장", "연결");
        else
            ui_dialog(UI_PASSWORD, visible[i].ssid, PASSWORD, "연결");
    }
    if (sscanf(e->name, "wifi saved %zu", &i) == 1 && network_count < 16)
        snprintf(networks[network_count++], sizeof networks[0], "%s", visible[i].ssid);
    if (strcmp(e->name, "wifi up") == 0 && i > 0 && i < network_count) {
        char swap[33];
        memcpy(swap, networks[i - 1], sizeof swap), memcpy(networks[i - 1], networks[i], sizeof swap), memcpy(networks[i], swap, sizeof swap);
    }
    if (strcmp(e->name, "wifi edit") == 0 && i < network_count) ui_dialog(UI_PASSWORD, networks[i], PASSWORD, "연결");
    if (strcmp(e->name, "wifi delete") == 0 && i < network_count) {
        snprintf(asked, sizeof asked, "wifi removed %zu", i);
        ui_dialog(UI_CONFIRM, networks[i], "삭제할까요?", "삭제");
    }
    if (sscanf(e->name, "wifi removed %zu", &i) == 1 && i < network_count)
        memmove(networks[i], networks[i + 1], (--network_count - i) * sizeof networks[0]);
}

static session_info* numbered(int number) {
    for (size_t i = 0; i < state.session_count; i++)
        if (state.sessions[i].number == number) return &state.sessions[i];
    return NULL;
}

// As main.c's sessions, without connections: done at once, a host and a delete confirmed first
static void sessions(const ui_event* e) {
    char verb[16];
    int number = 0;
    if (strcmp(e->name, "session create") == 0) {
        char question[96];
        snprintf(asked, sizeof asked, "create %zu", e->index);
        snprintf(question, sizeof question, "'%s'\n위 호스트의 세션을 생성할까요?", hosts[e->index].alias);
        ui_dialog(UI_CONFIRM, NULL, question, "생성");
        return;
    }
    size_t host;
    if (sscanf(e->name, "create %zu", &host) == 1 && state.session_count < MAX_SESSIONS) {
        bool taken[MAX_SESSIONS + 1] = {0};
        for (size_t i = 0; i < state.session_count; i++)
            taken[state.sessions[i].number] = true;
        while (taken[++number])
            ;
        size_t i = state.session_count++;
        while (i > 0 && state.sessions[i - 1].number > number)
            state.sessions[i] = state.sessions[i - 1], i--;
        state.sessions[i] = (session_info){.number = number, .alias = hosts[host].alias, .status = SESSION_DISCONNECTED};
        return;
    }
    if (sscanf(e->name, "removed %d", &number) == 1 && numbered(number) != NULL) {
        session_info* s = numbered(number);
        memmove(s, s + 1, (size_t)(state.sessions + --state.session_count - s) * sizeof *s);
        return;
    }
    bool tab = sscanf(e->name, "session %d", &number) == 1;
    if (!tab && sscanf(e->name, "session %15s", verb) != 1) return;
    if (!tab && e->index < state.session_count) number = state.sessions[e->index].number;
    session_info* s = numbered(number);
    if (s == NULL) return;
    if (tab && s->shown) {
        ui_open("sessions");
        return;
    }
    if (!tab && strcmp(verb, "delete") == 0) {
        char title[64];
        snprintf(asked, sizeof asked, "removed %d", number);
        snprintf(title, sizeof title, "세션 %d %s", number, s->alias);
        ui_dialog(UI_CONFIRM, title, "삭제할까요?\n서버의 tmux 세션도 함께 끝납니다", "삭제");
        return;
    }
    bool open = tab || strcmp(verb, "open") == 0, disconnect = !open && strcmp(verb, "toggle") == 0 && s->status != SESSION_DISCONNECTED;
    if (open || disconnect)
        for (size_t i = 0; i < state.session_count; i++)
            state.sessions[i].shown = open && &state.sessions[i] == s;
    s->status = disconnect ? SESSION_DISCONNECTED : SESSION_CONNECTED;
    for (size_t i = 0; open && i < sizeof hosts / sizeof hosts[0]; i++)
        if (strcmp(hosts[i].alias, s->alias) == 0) state.current_host = &hosts[i];
    if (open) ui_open(NULL);
}

// What main.c is to do with an event
static void handle(const ui_event* e) {
    if (e->kind == UI_ACTION && strncmp(e->name, "dialog ", 7) == 0) {
        char then[sizeof asked];
        bool pick = strcmp(e->name, "dialog pick") == 0;
        size_t picked = ui_dialog_chosen();
        snprintf(then, sizeof then, "%s", pick ? "wifi pick" : strcmp(e->name, "dialog ok") == 0 ? asked : "");
        asked[0] = 0;
        ui_dialog_close();
        if (then[0] != 0) handle(&(ui_event){.kind = UI_ACTION, .name = then, .index = pick ? picked : 0});
        snprintf(action, sizeof action, "%s", e->name);
    } else if (e->kind == UI_ACTION && strcmp(e->name, "wifi toggle") == 0) {
        state.wifi_enabled = !state.wifi_enabled, snprintf(action, sizeof action, "%s", e->name);
    } else if (e->kind == UI_ACTION && strcmp(e->name, "lte toggle") == 0) {
        state.lte_enabled = !state.lte_enabled, snprintf(action, sizeof action, "%s", e->name);
    } else if (e->kind == UI_ACTION && strncmp(e->name, "wifi ", 5) == 0) {
        wifi_screen(e);
        snprintf(action, sizeof action, "%s %zu", e->name, e->index);
    } else if (e->kind == UI_SET) {
        char value[80];
        bool ok = settings_set(&state, e->setting, &e->value);
        settings_text(e->setting, &e->value, value, sizeof value);
        snprintf(action, sizeof action, "set %s %s%s", e->name, value, ok ? "" : " (refused)");
    } else if (e->kind == UI_ACTION) {
        snprintf(action, sizeof action, "%s %zu", e->name, e->index);
        sessions(e);
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

// As main.c's main loop: the sessions screen stands for no session shown
static void tend(void) {
    static bool listing = true;
    bool seen = false;
    for (size_t i = 0; i < state.session_count; i++)
        seen |= state.sessions[i].shown;
    ui_home(seen ? NULL : "sessions");
    const char* panel = ui_panel();
    bool list = panel != NULL && strcmp(panel, "sessions") == 0;
    for (size_t i = 0; list && !listing && i < state.session_count; i++)
        state.sessions[i].shown = false;
    listing = list;
}

static void run_input(const char* script) {
    char copy[1024];
    snprintf(copy, sizeof copy, "%s", script);
    for (char *save, *step = strtok_r(copy, " ", &save); step != NULL; step = strtok_r(NULL, " ", &save)) {
        int column, row;
        ui_event e = {.kind = UI_NONE};
        if (sscanf(step, "tap:%d,%d", &column, &row) == 2) {
            e = ui_touch(UI_TOUCH_DOWN, column, row);
            handle(&e);
            e = ui_touch(UI_TOUCH_UP, column, row);
        } else if (strncmp(step, "text:", 5) == 0) {
            e = ui_key(UI_KEY_TEXT, step + 5);
        } else if (strcmp(step, "dialog:alert") == 0) {
            ui_dialog(UI_ALERT, "세션 2 nas", "서버가 거부함 · 끊음", "확인");
        } else if (strcmp(step, "dialog:confirm") == 0) {
            ui_dialog(UI_CONFIRM, "처음 보는 호스트 nas.lan", "SHA256:Uq5Ow1Ql0hKdM2dQ3s9sQe3ZQfZQ0d0qk6m1n2o3p4q\n이 키를 믿고 연결할까요?", "연결");
        } else if (strcmp(step, "dialog:prompt") == 0) {
            ui_dialog(UI_PASSWORD, "admin@nas.lan", "비밀번호", "확인");
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
        tend();
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
    // A disconnected session and a second one that waits for a password
    state.sessions[0] = (session_info){.number = 1, .alias = hosts[0].alias, .status = SESSION_DISCONNECTED};
    state.sessions[1] = (session_info){.number = 2, .alias = hosts[1].alias, .status = SESSION_ASKING};
    state.session_count = 2;
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
    // As at boot
    ui_open("sessions");
    tend();
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
        tend();
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
