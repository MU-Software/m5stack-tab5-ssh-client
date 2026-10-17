// SPDX-License-Identifier: GPL-3.0-or-later
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs_flash.h>
#include <wolfssl/wolfcrypt/memory.h>

#include "catm.h"
#include "clock.h"
#include "config.h"
#include "keyboard_tab5.h"
#include "keyboard_usb.h"
#include "keys.h"
#include "logs.h"
#include "memory_log.h"
#include "modem.h"
#include "net.h"
#include "power.h"
#include "remote.h"
#include "screen.h"
#include "settings.h"
#include "ssh.h"
#include "storage.h"
#include "terminal.h"
#include "tmux.h"
#include "update.h"
#include "usb.h"

static const char* TAG = "main";
static global_state state = {
    .terminal_type = "xterm-256color",
    .terminal_size = {.columns = 106, .rows = 30, .width_pixels = 1272, .height_pixels = 720},
    .log = {.enabled = true, .file_kb = 1024, .keep_mb = 32, .keep_days = 14},
    .timezone = "KST-9",
    .echo = {true, 50},
    .bars = {true, true},
    .touch = {48, true},
};

// Bumped by each change of the terminal's size; the host chosen on the screen at boot, NULL when none waits
static atomic_uint resizes;
static _Atomic(const ssh_host*) wanted_host;

// An SSH session with its own terminal, kept connected by its own task. The main loop opens, shows and ends them, and keeps
// them in NVS across restarts; session number n is sessions[n - 1].
typedef struct {
    // Tells its NVS saves from those of a session opened before under the same number
    unsigned id;
    terminal* terminal;
    // Set once, by its task for the first session chosen at boot
    _Atomic(const ssh_host*) host;
    // Its tmux session, "" before one runs; written by its task under names_lock
    char tmux[24];
    ssh_session connection;
    _Atomic(session_status) status;
    // What it waits for and since when, shown while it is
    _Atomic(const char*) waiting;
    atomic_uint waiting_since;
    // Asked to end; its task has ended
    atomic_bool end, ended;
    bool used;
    // The session's end ended its tmux session
    bool tmux_ended;
} session;
static session sessions[MAX_SESSIONS];
static _Atomic(session*) shown;
// keys_read and its Hangul composition serve one task at a time
static SemaphoreHandle_t input_lock, names_lock;
// This device's tmux sessions are named tab5-MMMMMM-RRRRRR: the MAC's last three bytes, then random
static char tmux_prefix[16];
// Sessions connected: while any is, logs may be quieter
static atomic_int connected;

static int number(const session* s) {
    return (int)(s - sessions) + 1;
}

static bool is_shown(const session* s) {
    return atomic_load(&shown) == s;
}

static void message(session* s, const char* text) {
    terminal_write(s->terminal, text, strlen(text));
    if (is_shown(s)) usb_terminal_write(text, strlen(text));
}

// From the keyboards or the USB terminal port, only for the shown session, which then shows the bottom of its scrollback
static size_t read_input(const session* s, void* buffer, size_t size) {
    size_t n = 0;
    if (!is_shown(s)) return 0;
    xSemaphoreTake(input_lock, portMAX_DELAY);
    if (is_shown(s) && (n = keys_read(buffer, size)) == 0) n = usb_terminal_read(buffer, size);
    xSemaphoreGive(input_lock);
    if (n > 0) terminal_scroll_to_bottom(s->terminal);
    return n;
}

// As screen_waiting, for the screen while s is shown
static void waiting(session* s, const char* what, TickType_t since) {
    if (what == NULL || what != atomic_load(&s->waiting)) atomic_store(&s->waiting_since, since);
    atomic_store(&s->waiting, what);
    if (is_shown(s)) screen_waiting(what, since);
}

// On the session's terminal, typed once it is shown; empty when the session is to end
static void read_line(session* s, const char* prompt, char* buf, size_t size, int echo) {
    session_status was = atomic_exchange(&s->status, SESSION_ASKING);
    message(s, prompt);
    size_t n = 0;
    while (!atomic_load(&s->end)) {
        char c;
        if (read_input(s, &c, 1) != 1) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == '\r' || c == '\n') break;
        if (c == 0x7f || c == '\b') {
            if (n == 0) continue;
            n--;
            if (echo) message(s, "\b \b");
        } else if (n + 1 < size) {
            buf[n++] = c;
            if (echo) message(s, (char[]){c, 0});
        }
    }
    buf[n] = 0;
    message(s, "\r\n");
    atomic_store(&s->status, was);
}

int accept_new_host(const ssh_session* connection, const char* name, const char* fingerprint, const char* line) {
    session* s = NULL;
    for (size_t i = 0; i < MAX_SESSIONS; i++)
        if (&sessions[i].connection == connection) s = &sessions[i];
    if (s == NULL) return 0;
    char prompt[512], answer[8];
    snprintf(prompt, sizeof prompt,
             "The authenticity of host '%s' can't be established.\r\nKey fingerprint is %s.\r\n"
             "Are you sure you want to continue connecting (yes/no)? ",
             name, fingerprint);
    read_line(s, prompt, answer, sizeof answer, 1);
    if (strcmp(answer, "yes") != 0) return 0;
    if (storage_add_known_host(line) != 0) ESP_LOGW(TAG, "could not add %s to " SD "/.ssh/known_hosts", name);
    return 1;
}

typedef enum { SESSION_CLOSED, SESSION_REFUSED, SESSION_DROPPED, SESSION_MOVED, SESSION_ENDED } session_end;

// Typing that 1 s brings no output pings the host; no answer for another second shows as held until anything arrives.
// The end of a tmux session's session first tells tmux to kill it, which closes the channel, for up to 3 s. Mouse reports
// count as typing; those from before the connection are dropped.
static session_end bridge(session* s, unsigned path, bool tmux_session) {
    uint8_t in[256], out[1024];
    int pending = 0, off = 0;
    TickType_t typed_at = 0, pinged_at = 0, ending = 0;
    bool typed = false, pinged = false, held = false;
    unsigned pongs = 0, size = atomic_load(&resizes);
    terminal_read_mouse(s->terminal, out, sizeof out);
    for (;;) {
        if (net_generation() != path) return SESSION_MOVED;
        if (atomic_load(&s->end) && (!tmux_session || (ending != 0 && xTaskGetTickCount() - ending >= pdMS_TO_TICKS(3000)))) return SESSION_ENDED;
        if (atomic_load(&s->end) && ending == 0) {
            ending = xTaskGetTickCount() | 1;
            pending = (int)strlen(tmux_end_keys), off = 0;
            memcpy(in, tmux_end_keys, (size_t)pending);
        }
        if (atomic_load(&resizes) != size) size = atomic_load(&resizes), ssh_resize(&state, &s->connection);
        if (pending == 0) {
            if ((pending = (int)terminal_reply(s->terminal, in, sizeof in)) == 0 &&
                ((pending = (int)read_input(s, in, sizeof in)) > 0 || (pending = (int)terminal_read_mouse(s->terminal, in, sizeof in)) > 0)) {
                screen_typed(in, (size_t)pending);
                if (!typed) typed = true, typed_at = xTaskGetTickCount();
            }
            off = 0;
        }
        int sent = pending > 0 ? ssh_write(&s->connection, in + off, (size_t)pending) : WS_WANT_WRITE;
        int got = ssh_read(&s->connection, out, sizeof out);
        s->tmux_ended |= ending != 0 && got == WS_CHANNEL_CLOSED;
        if (got == WS_CHANNEL_CLOSED) return SESSION_CLOSED;
        if ((sent < 0 && sent != WS_WANT_WRITE) || (got < 0 && got != WS_WANT_READ)) return SESSION_DROPPED;
        if (sent > 0) {
            off += sent;
            pending -= sent;
        }
        TickType_t now = xTaskGetTickCount();
        if (got > 0 || (pinged && s->connection.pongs != pongs)) {
            typed = pinged = false;
            if (held) waiting(s, NULL, 0), held = false;
        } else if (typed && !pinged && now - typed_at >= pdMS_TO_TICKS(1000)) {
            pongs = s->connection.pongs, pinged = true, pinged_at = now;
            ssh_ping(&s->connection);
        } else if (pinged && !held && now - pinged_at >= pdMS_TO_TICKS(1000)) {
            waiting(s, "held", typed_at);
            held = true;
        }
        if (got > 0) {
            terminal_write(s->terminal, out, (size_t)got);
            if (is_shown(s)) usb_terminal_write(out, (size_t)got);
        } else if (sent < 0)
            ssh_wait(&s->connection, 10);
    }
}

// The exit status counts only for SESSION_CLOSED
static session_end run(session* s, const ssh_host* h, const char* command, bool tmux_session, unsigned path, int* exit_status) {
    char* private_key = storage_get_private_key(h->identity_file);
    char password[128] = "";
    if (private_key == NULL) {
        char prompt[300];
        snprintf(prompt, sizeof prompt, "%s@%s's password: ", h->user, h->hostname);
        read_line(s, prompt, password, sizeof password, 0);
    }
    char* known_hosts = storage_get_known_hosts();
    s->connection = (ssh_session){.host = h,
                                  .known_hosts = known_hosts,
                                  .private_key = private_key,
                                  .password = password[0] ? password : NULL,
                                  .command = command,
                                  .local_address = net_local_address(),
                                  .timeout_ms = net_on_lte() ? 30000 : 10000};
    atomic_store(&s->status, SESSION_CONNECTING);
    int rc = ssh_open(&state, &s->connection);
    if (private_key != NULL) wc_ForceZero(private_key, strlen(private_key));
    free(private_key);
    free(known_hosts);
    memset(password, 0, sizeof password);
    // Refused: retrying the same keys and host key would only be refused again
    session_end end = rc == WS_SOCKET_ERROR_E ? SESSION_DROPPED : SESSION_REFUSED;
    *exit_status = 0;
    if (rc == WS_SUCCESS) {
        net_keepalive(s->connection.fd);
        waiting(s, NULL, 0);
        atomic_store(&s->status, SESSION_CONNECTED);
        ESP_LOGI(TAG, "session %d connected to %s: %s", number(s), h->hostname, command != NULL ? command : "shell");
        if (atomic_fetch_add(&connected, 1) == 0 && state.log.quiet_sessions) {
            esp_log_level_set("*", ESP_LOG_WARN);
            // They answer control commands
            for (size_t i = 0; i < 5; i++)
                esp_log_level_set((const char* const[]){"catm", "main", "net", "power", "wifi"}[i], ESP_LOG_INFO);
        }
        end = bridge(s, path, tmux_session);
        if (is_shown(s)) screen_typed(NULL, 0);
        if (atomic_fetch_sub(&connected, 1) == 1) esp_log_level_set("*", ESP_LOG_INFO);
        ssh_close(&s->connection, exit_status);
        atomic_store(&s->status, SESSION_CONNECTING);
        ESP_LOGI(TAG, "session %d %s, exit status %d", number(s), (const char* const[]){"closed", "refused", "dropped", "moved", "ended"}[end],
                 *exit_status);
    } else {
        ESP_LOGE(TAG, "session %d: ssh %s@%s:%u: %s", number(s), h->user, h->hostname, h->port, wolfSSH_ErrorToName(rc));
    }
    return end;
}

// NVS writes from the session tasks, whose stacks are in PSRAM: writing flash needs an internal stack, main's loop does them
typedef struct {
    // Of session `number`, or the last host for 0
    unsigned id;
    int number;
    const ssh_host* host;
    char tmux[24];
} save;
static QueueHandle_t saves;

static void save_session(const session* s) {
    save v = {s->id, number(s), atomic_load(&s->host), ""};
    memcpy(v.tmux, s->tmux, sizeof v.tmux);
    xQueueSend(saves, &v, portMAX_DELAY);
}

static void set_tmux(session* s, const char* name) {
    xSemaphoreTake(names_lock, portMAX_DELAY);
    snprintf(s->tmux, sizeof s->tmux, "%s", name);
    xSemaphoreGive(names_lock);
    save_session(s);
}

// " a b ": the tmux sessions of the open sessions to h, which a connection to h leaves; those of sessions to end not
static void kept(const ssh_host* h, char* out, size_t size) {
    size_t n = (size_t)snprintf(out, size, " ");
    xSemaphoreTake(names_lock, portMAX_DELAY);
    for (size_t i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].used && !atomic_load(&sessions[i].end) && atomic_load(&sessions[i].host) == h && sessions[i].tmux[0] != 0 && n < size)
            n += (size_t)snprintf(out + n, size - n, "%s ", sessions[i].tmux);
    xSemaphoreGive(names_lock);
}

// Attaches to the session's tmux session, or starts one when there is none or it has gone; a name from before the
// device's prefix is renamed into it. Every connection also kills this device's tmux sessions on the host that no open
// session uses, unattached for a minute: those of sessions ended while the host could not be reached.
static session_end tmux(session* s, const ssh_host* h, unsigned path) {
    char name[24], old[24], keep[MAX_SESSIONS * 24 + 2], command[1400];
    snprintf(old, sizeof old, "%s", s->tmux);
    bool attach = old[0] != 0, renamed = attach && strncmp(old, tmux_prefix, strlen(tmux_prefix)) != 0;
    for (;;) {
        if (!attach || renamed) {
            snprintf(name, sizeof name, "%s%06" PRIx32, tmux_prefix, esp_random() & 0xffffff);
            set_tmux(s, name);
        }
        kept(h, keep, sizeof keep);
        if (!tmux_command(command, sizeof command, s->tmux, attach, renamed ? old : NULL, tmux_prefix, keep, 60, state.touch.arrows))
            return SESSION_REFUSED;
        int exit_status;
        session_end end = run(s, h, command, true, path, &exit_status);
        if (end != SESSION_CLOSED || atomic_load(&s->end)) return end;
        if (exit_status == 127) {
            set_tmux(s, "");
            message(s, "\r\n\x1b[33mNo tmux on this host: a plain shell, lost when the connection drops\x1b[m\r\n");
            return run(s, h, NULL, false, path, &exit_status);
        }
        if (!attach || exit_status == 0) return end;
        attach = renamed = false;
    }
}

// Or until the session is to end
static void wait_enter(session* s) {
    while (!atomic_load(&s->end)) {
        char in[64];
        size_t n = read_input(s, in, sizeof in);
        if (memchr(in, '\r', n) != NULL) return;
        if (n == 0) vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Until there is a path, what LTE is doing; Enter tries LTE at once, the other keys are dropped. False when the session
// is to end.
static bool wait_path(session* s, unsigned* path) {
    static const char* const states[] = {
        [NET_LTE_OFF] = "no network: LTE off, Enter turns it on",
        [NET_LTE_NO_MODEM] = "no network: no LTE modem",
        [NET_LTE_DIALING] = "no network: LTE dialing",
        [NET_LTE_REGISTERING] = "no network: LTE not registered",
        [NET_LTE_WAITING] = "no network: LTE waits, Enter retries",
        [NET_LTE_UP] = "no network",
    };
    TickType_t since = xTaskGetTickCount();
    while (!net_wait_for(200, path)) {
        if (atomic_load(&s->end)) return false;
        char in[64];
        size_t n = read_input(s, in, sizeof in);
        if (memchr(in, '\r', n) != NULL) net_command("lte on");
        waiting(s, states[net_lte()], since);
    }
    return !atomic_load(&s->end);
}

// Reconnects after a drop or a change of path at once (tmux keeps the screen), after a closed session on Enter; until the
// session is to end
static void keep_connected(session* s) {
    int retry_ms = 1000;
    unsigned path;
    while (wait_path(s, &path)) {
        session_end end = tmux(s, atomic_load(&s->host), path);
        if (end == SESSION_ENDED) break;
        waiting(s, end == SESSION_DROPPED || end == SESSION_MOVED ? "reconnecting" : NULL, xTaskGetTickCount());
        if (end == SESSION_CLOSED || end == SESSION_REFUSED) {
            atomic_store(&s->status, SESSION_STOPPED);
            message(s, end == SESSION_CLOSED ? "\r\n[Session closed. Enter connects again.]\r\n" : "\r\n[Not connected. Enter tries again.]\r\n");
            wait_enter(s);
            atomic_store(&s->status, SESSION_CONNECTING);
        } else if (end == SESSION_DROPPED) {
            net_suspect();
            // A new path is worth trying at once
            for (unsigned at = net_generation(), waited = 0; waited < retry_ms && net_generation() == at && !atomic_load(&s->end); waited += 100)
                vTaskDelay(pdMS_TO_TICKS(100));
            retry_ms = retry_ms < 16000 ? retry_ms * 2 : 30000;
            continue;
        }
        retry_ms = 1000;
    }
}

// The host last connected to; with more than one, the screen offers them all, and after 10 s untouched connects
static const ssh_host* choose_host(void) {
    char last[sizeof state.hosts->alias];
    storage_get_last_host(last, sizeof last);
    for (const ssh_host* h = state.hosts; h != NULL; h = h->next)
        if (strcmp(h->alias, last) == 0) state.current_host = h;
    if (state.hosts == NULL || state.hosts->next == NULL) return state.current_host;
    screen_input();
    state.host_deadline_us = esp_timer_get_time() + 10 * 1000000LL;
    screen_open("sessions");
    while (screen_panel_open() && atomic_load(&wanted_host) == NULL) {
        if (state.host_deadline_us != 0 && screen_input()) state.host_deadline_us = 0;
        if (state.host_deadline_us != 0 && esp_timer_get_time() >= state.host_deadline_us) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    state.host_deadline_us = 0;
    const ssh_host* chosen = atomic_exchange(&wanted_host, NULL);
    if (chosen != NULL) state.current_host = chosen;
    if (state.current_host != NULL) xQueueSend(saves, &(save){.host = state.current_host}, portMAX_DELAY);
    screen_open(NULL);
    return state.current_host;
}

static void refused(const char* name) {
    ESP_LOGE(TAG, "the configuration's %s is invalid", name);
}

// Before the screen and the network start: the bars, the networks and the host to check them against
static void load_config(void) {
    static const char* const sources[] = {"nothing", "the SD card", "NVS"};
    config_source source;
    cJSON* config = storage_get_device_config(&source);
    if (config_wifi_networks(config, &state.wifi) != 0)
        ESP_LOGE(TAG, "the configuration from %s is invalid", sources[source]);
    else
        ESP_LOGI(TAG, "configuration from %s", sources[source]);
    settings_load(&state, config, refused);
    cJSON_Delete(config);
    clock_timezone(state.timezone);
    logs_configure(&state.log);
    char* text = storage_get_ssh_config();
    state.hosts = text != NULL ? config_ssh_hosts(text) : NULL;
    free(text);
    state.current_host = state.hosts;
    state.wifi_off = storage_get_off("wifi");
    state.lte_off = storage_get_off("lte");
}

// wifi connect SSID [PASSWORD], "SSID" when it has spaces; saved once joined
static void wifi_connect_command(const char* arguments) {
    wifi_network* network = calloc(1, sizeof *network);
    if (network == NULL) return;
    const char* end = arguments[0] == '"' ? strchr(arguments + 1, '"') : strchr(arguments, ' ');
    const char* ssid = arguments + (arguments[0] == '"');
    size_t length = end != NULL ? (size_t)(end - ssid) : strlen(ssid);
    if (length == 0 || length >= sizeof network->ssid) {
        free(network);
        return;
    }
    memcpy(network->ssid, ssid, length);
    const char* password = end == NULL ? "" : end + 1;
    while (*password == ' ')
        password++;
    strlcpy(network->password, password, sizeof network->password);
    // It turns Wi-Fi on, as wifi on does
    if (state.wifi_off) storage_set_off("wifi", state.wifi_off = false);
    esp_err_t err = net_join(network);
    ESP_LOGI(TAG, "wifi connect %s: %s", network->ssid, esp_err_to_name(err));
    if (err == ESP_OK && storage_add_wifi(network->ssid, network->password) != 0) ESP_LOGW(TAG, "%s not saved", network->ssid);
    if (err != ESP_OK) {
        memset(network, 0, sizeof *network);
        free(network);
        return;
    }
    // Prepended and never freed: the network task may be walking the list
    network->next = state.wifi;
    state.wifi = network;
    net_wifi_changed();
}

// The Wi-Fi screen's join takes seconds: on its own task, and saved by the main loop once joined
typedef enum { JOIN_IDLE, JOIN_RUNNING, JOIN_DONE } join_step;
static struct {
    char saved[33];
    wifi_network network;
    esp_err_t error;
    _Atomic join_step step;
} join;

static void join_wifi(void* arg) {
    (void)arg;
    join.error = net_join(&join.network);
    atomic_store(&join.step, JOIN_DONE);
    vTaskDeleteWithCaps(NULL);
}

// Network ssid of the configuration in use changed, and the list the network task joins from with it; the old list is
// kept, as that task may be walking it
static bool save_wifi(const char* ssid, const wifi_network* network, int move) {
    wifi_network* networks;
    bool saved = storage_set_wifi(ssid, network, move, &networks) == 0;
    if (saved) state.wifi = networks, net_wifi_changed();
    ESP_LOGI(TAG, "wifi %s %s%s",
             network == NULL ? "delete"
             : move != 0     ? "up"
                             : "save",
             network != NULL ? network->ssid : ssid, saved ? "" : ": not saved");
    return saved;
}

// The screen's network, once saved, is changed as a saved one
static const char* saved_wifi(wifi_edit* w, const char* ssid, const char* result) {
    if (strcmp(w->network.ssid, ssid) == 0) snprintf(w->saved, sizeof w->saved, "%s", ssid);
    return result;
}

static void joined_wifi(void) {
    wifi_edit* w = &state.wifi_edit;
    ESP_LOGI(TAG, "wifi join %s: %s", join.network.ssid, esp_err_to_name(join.error));
    if (join.error == ESP_OK)
        w->result = save_wifi(join.saved[0] ? join.saved : join.network.ssid, &join.network, 0) ? saved_wifi(w, join.network.ssid, "연결됨, 저장함")
                                                                                                : "연결됨, 저장 못 함";
    else if (join.error == ESP_ERR_NOT_FOUND)
        w->result = "보이지 않음, 저장 안 함";
    else if (join.error == ESP_ERR_INVALID_STATE)
        w->result = "Wi-Fi가 시작되지 않음";
    else
        w->result = "연결 못 함, 저장 안 함";
    memset(&join, 0, sizeof join);
}

static const wifi_network* saved_at(size_t index) {
    for (const wifi_network* w = state.wifi; w != NULL; w = w->next)
        if (w->ssid[0] != 0 && index-- == 0) return w;
    return NULL;
}

static const wifi_network* saved_named(const char* ssid) {
    for (const wifi_network* w = state.wifi; ssid[0] != 0 && w != NULL; w = w->next)
        if (strcmp(w->ssid, ssid) == 0) return w;
    return NULL;
}

// The Wi-Fi screen: a saved network to change, or a new one picked from a scan or typed; saved as typed, or once joined
static bool wifi_screen(const ui_event* e) {
    wifi_edit* w = &state.wifi_edit;
    static const char* const actions[] = {"add", "edit", "scan", "pick", "save", "join", "up", "delete"};
    const char* action = e->kind == UI_ACTION && strncmp(e->name, "wifi ", 5) == 0 ? e->name + 5 : "";
    const wifi_network* saved = strcmp(action, "edit") == 0 ? saved_at(e->index) : saved_named(w->saved);
    bool ssid = e->kind == UI_SET && strcmp(e->name, "wifi.ssid") == 0, deleting = w->deleting;
    size_t known = 0;
    while (known < sizeof actions / sizeof actions[0] && strcmp(action, actions[known]) != 0)
        known++;
    if (ssid || (e->kind == UI_SET && strcmp(e->name, "wifi.password") == 0)) {
        snprintf(ssid ? w->network.ssid : w->network.password, ssid ? sizeof w->network.ssid : sizeof w->network.password, "%s", e->value.text);
        w->result = NULL, w->deleting = false;
        return true;
    }
    if (strcmp(action, "add") == 0 || strcmp(action, "edit") == 0) {
        if (action[0] == 'e' && saved == NULL) return true;
        memset(w, 0, sizeof *w);
        if (saved != NULL) {
            memcpy(w->saved, saved->ssid, sizeof w->saved);
            memcpy(w->network.ssid, saved->ssid, sizeof w->network.ssid);
            memcpy(w->network.password, saved->password, sizeof w->network.password);
        } else {
            net_scan();
        }
        screen_open("wifi");
        return true;
    }
    if (known == sizeof actions / sizeof actions[0]) return false;
    w->result = NULL;
    if (strcmp(action, "scan") == 0) {
        net_scan();
    } else if (strcmp(action, "pick") == 0) {
        net_visible_network v;
        if (net_visible(e->index, &v)) snprintf(w->network.ssid, sizeof w->network.ssid, "%s", v.ssid);
    } else if ((strcmp(action, "save") == 0 || strcmp(action, "join") == 0) && !config_wifi_valid(&w->network)) {
        w->result = "SSID 1~32자, 비밀번호는 없거나 8~63자";
    } else if (strcmp(action, "save") == 0) {
        w->result = save_wifi(w->saved[0] ? w->saved : w->network.ssid, &w->network, 0) ? saved_wifi(w, w->network.ssid, "저장함") : "저장 못 함";
    } else if (strcmp(action, "join") == 0 && atomic_load(&join.step) != JOIN_IDLE) {
        w->result = "다른 연결을 기다리는 중";
    } else if (strcmp(action, "join") == 0) {
        // It turns Wi-Fi on, as wifi connect does
        if (state.wifi_off) storage_set_off("wifi", state.wifi_off = false);
        memcpy(join.saved, w->saved, sizeof join.saved);
        join.network = w->network;
        atomic_store(&join.step, JOIN_RUNNING);
        w->result = "연결 중…";
        if (xTaskCreatePinnedToCoreWithCaps(join_wifi, "join", 6144, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
            memset(&join, 0, sizeof join);
            w->result = "연결 못 함";
        }
    } else if (strcmp(action, "up") == 0) {
        w->result = saved == NULL ? NULL : saved == saved_at(0) ? "이미 맨 위" : save_wifi(saved->ssid, saved, -1) ? "한 칸 위로" : "저장 못 함";
    } else if (strcmp(action, "delete") == 0 && !deleting) {
        w->result = saved != NULL ? "한 번 더 누르면 삭제" : NULL;
    } else if (strcmp(action, "delete") == 0) {
        if (saved != NULL && save_wifi(saved->ssid, NULL, 0))
            screen_open("network");
        else
            w->result = "삭제 못 함";
    }
    w->deleting = !deleting && saved != NULL && strcmp(action, "delete") == 0;
    return true;
}

// From the screen or the set command
static void change(const setting* s, const setting_value* value) {
    char text[80];
    settings_text(s, value, text, sizeof text);
    if (!settings_set(&state, s, value)) {
        ESP_LOGW(TAG, "%s %s refused", s->name, text);
        return;
    }
    bool saved = storage_set_config(s->name, settings_json(&state, s)) == 0;
    ESP_LOGI(TAG, "%s %s%s", s->name, text, saved ? "" : ", not saved");
    if (strncmp(s->name, "log.", 4) == 0) logs_configure(&state.log);
    if (strcmp(s->name, "timezone") == 0) clock_timezone(state.timezone);
}

// set NAME VALUE, the value as the configuration file keeps it, text also without its quotes
static void set_command(const char* arguments) {
    const char* value = strchr(arguments, ' ');
    char name[32];
    snprintf(name, sizeof name, "%.*s", value != NULL ? (int)(value - arguments) : 0, arguments);
    const setting* s = value != NULL ? settings_find(name) : NULL;
    cJSON* item = s != NULL ? cJSON_ParseWithOpts(value + 1, NULL, true) : NULL;
    if (s != NULL && item == NULL) item = cJSON_CreateString(value + 1);
    setting_value v;
    if (item != NULL && settings_parse(s, item, &v))
        change(s, &v);
    else
        ESP_LOGW(TAG, "set %s: no such setting or not a value for it", arguments);
    cJSON_Delete(item);
}

static void log_modem(void) {
    modem_snapshot* s = heap_caps_malloc(sizeof *s, MALLOC_CAP_SPIRAM);
    if (s == NULL) return;
    catm_snapshot(s);
    int64_t now = esp_timer_get_time();
    if (catm_connected()) ESP_LOGI(TAG, "modem: PPP is up, values from before the dial (lte off to read again)");
    char logged[sizeof s->values[0]];
    for (int i = 0; i < MODEM_ITEMS; i++) {
        if (s->read_at[i] == 0) continue;
        modem_mask(s->values[i], logged, sizeof logged);
        ESP_LOGI(TAG, "modem %s: %s (%lld s ago)", modem_commands[i].name, logged, (now - s->read_at[i]) / 1000000);
    }
    free(s);
}

static atomic_bool modem_reading;

static void read_modem(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "modem read: %s", esp_err_to_name(catm_read()));
    log_modem();
    atomic_store(&modem_reading, false);
    vTaskDeleteWithCaps(NULL);
}

// modem status|read; a read waits for the modem, which a dial may hold for a minute, off the screen's task
static void modem_control(const char* arguments) {
    if (strcmp(arguments, "status") == 0) {
        log_modem();
    } else if (strcmp(arguments, "read") != 0) {
        ESP_LOGW(TAG, "modem status|read");
    } else if (atomic_exchange(&modem_reading, true)) {
        ESP_LOGW(TAG, "modem: reading already");
    } else if (xTaskCreatePinnedToCoreWithCaps(read_modem, "modem", 4096, NULL, 4, NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        // Stack in PSRAM: a task there must never write flash
        atomic_store(&modem_reading, false);
    }
}

static bool command(const char* line) {
    if (strncmp(line, "modem ", 6) == 0) {
        modem_control(line + 6);
        return true;
    }
    if (strcmp(line, "settings") == 0) {
        const setting* s;
        for (size_t i = 0; (s = settings_at(i)) != NULL; i++) {
            cJSON* value = settings_json(&state, s);
            char* text = cJSON_PrintUnformatted(value);
            ESP_LOGI(TAG, "%s %s", s->name, text != NULL ? text : "?");
            free(text);
            cJSON_Delete(value);
        }
        return true;
    }
    if (strncmp(line, "set ", 4) == 0) {
        set_command(line + 4);
        return true;
    }
    if (strncmp(line, "wifi connect ", 13) == 0) {
        wifi_connect_command(line + 13);
        return true;
    }
    if (strcmp(line, "power") == 0) {
        power_report();
        return true;
    }
    // wifi off and lte off alone last the boot
    if (strcmp(line, "wifi off save") == 0 || strcmp(line, "wifi on") == 0) {
        state.wifi_off = strcmp(line, "wifi on") != 0;
        storage_set_off("wifi", state.wifi_off);
        net_wifi_changed();
        return net_command(state.wifi_off ? "wifi off" : "wifi on");
    }
    if (strcmp(line, "lte off save") == 0 || strcmp(line, "lte on") == 0) {
        state.lte_off = strcmp(line, "lte on") != 0;
        storage_set_off("lte", state.lte_off);
        return net_command(state.lte_off ? "lte off" : "lte on");
    }
    if (strncmp(line, "wifi forget ", 12) == 0) {
        // Emptied, not unlinked, for the same reason
        for (wifi_network* w = state.wifi; w != NULL; w = w->next)
            if (strcmp(w->ssid, line + 12) == 0) w->ssid[0] = 0;
        ESP_LOGI(TAG, "wifi forget %s: %s", line + 12, storage_add_wifi(line + 12, NULL) == 0 ? "done" : "not saved");
        net_wifi_changed();
        return true;
    }
    return net_command(line);
}

// The session the main loop shows from now on
static void show(session* s) {
    if (is_shown(s)) return;
    atomic_store(&shown, s);
    terminal_show(s->terminal);
    screen_show(s->terminal);
    screen_waiting(NULL, 0);
    screen_waiting(atomic_load(&s->waiting), (TickType_t)atomic_load(&s->waiting_since));
    if (atomic_load(&s->host) != NULL) state.current_host = atomic_load(&s->host);
}

static void run_session(void* arg) {
    session* s = arg;
    const ssh_host* h = atomic_load(&s->host);
    // The first, at boot
    if (h == NULL) {
        ESP_LOGI(TAG, "internal DMA RAM: %u free, %u in one block", heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                 heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if ((h = choose_host()) != NULL) atomic_store(&s->host, h);
    }
    if (h == NULL || !h->user[0])
        ESP_LOGE(TAG, "incomplete configuration: a Host with a User in the SD card's .ssh/config");
    else
        keep_connected(s);
    if (atomic_load(&s->end) && s->tmux[0] != 0)
        ESP_LOGI(TAG, "session %d: tmux session %s %s", number(s), s->tmux,
                 s->tmux_ended ? "ended" : "left on the host, for the next connection to it to end");
    atomic_store(&s->status, SESSION_STOPPED);
    atomic_store(&s->ended, true);
    vTaskDeleteWithCaps(NULL);
}

// Session n (the first free one for 0) to h, or the boot's host when NULL, on tmux session `tmux` ("" for a new
// one); NULL when all sessions are open or memory is short
static session* open_session(int n, const ssh_host* h, const char* tmux) {
    static unsigned opened;
    session* s = n > 0 ? &sessions[n - 1] : sessions;
    while (n == 0 && s < sessions + MAX_SESSIONS && s->used)
        s++;
    terminal* t = s < sessions + MAX_SESSIONS && !s->used ? terminal_new(&state.terminal_size) : NULL;
    if (t == NULL) return NULL;
    xSemaphoreTake(names_lock, portMAX_DELAY);
    memset(s, 0, sizeof *s);
    s->used = true, s->id = ++opened, s->terminal = t;
    atomic_store(&s->host, h);
    snprintf(s->tmux, sizeof s->tmux, "%s", tmux);
    xSemaphoreGive(names_lock);
    char name[24];
    snprintf(name, sizeof name, "session %d", number(s));
    // Ghostty's measured stack is 27-33 KiB, as fast in PSRAM; flash writes go through saves
    if (xTaskCreatePinnedToCoreWithCaps(run_session, name, 48 * 1024, s, 5, NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        terminal_free(t);
        s->used = false;
        return NULL;
    }
    ESP_LOGI(TAG, "session %d opened%s%s%s%s", number(s), h != NULL ? " to " : "", h != NULL ? h->alias : "", tmux[0] != 0 ? ", tmux " : "", tmux);
    return s;
}

static bool is_open(const session* s) {
    return s->used && !atomic_load(&s->end);
}

// Open session `index` in the screens' order, NULL past the last
static session* open_at(size_t index) {
    for (size_t i = 0; i < MAX_SESSIONS; i++)
        if (is_open(&sessions[i]) && index-- == 0) return &sessions[i];
    return NULL;
}

// Its task closes the connection and stops; the last one open stays
static void end_session(session* s) {
    session* other = NULL;
    for (size_t i = 0; i < MAX_SESSIONS; i++)
        if (&sessions[i] != s && is_open(&sessions[i]) && (other == NULL || &sessions[i] < s)) other = &sessions[i];
    if (other == NULL) return;
    if (is_shown(s)) show(other);
    atomic_store(&s->end, true);
    storage_set_session(number(s), NULL, NULL);
    ESP_LOGI(TAG, "session %d ending", number(s));
}

// Frees the sessions whose tasks ended as asked, and tells the screens about the others
static void tend_sessions(void) {
    state.session_count = 0;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        session* s = &sessions[i];
        if (s->used && atomic_load(&s->end) && atomic_load(&s->ended)) {
            terminal_free(s->terminal);
            s->used = false;
            ESP_LOGI(TAG, "session %d ended", number(s));
        }
        if (!is_open(s)) continue;
        if (is_shown(s)) state.shown_session = state.session_count;
        state.sessions[state.session_count++] = (session_info){number(s), atomic_load(&s->host), atomic_load(&s->status)};
    }
}

// The sessions open before the restart, on their tmux sessions; the first, NULL for none. Those whose host left the
// configuration are forgotten.
static session* restore_sessions(void) {
    session* first = NULL;
    storage_migrate_sessions();
    for (int n = 1; n <= MAX_SESSIONS; n++) {
        char alias[sizeof state.hosts->alias], name[24];
        const ssh_host* h = NULL;
        if (storage_get_session(n, alias, sizeof alias, name, sizeof name) != ESP_OK) continue;
        for (h = state.hosts; h != NULL && strcmp(h->alias, alias) != 0; h = h->next)
            ;
        session* s = h != NULL ? open_session(n, h, name) : NULL;
        if (h == NULL) storage_set_session(n, NULL, NULL);
        if (first == NULL) first = s;
    }
    return first;
}

// From the screen's task, whose stack in internal RAM lets it write NVS
static void apply(const ui_event* e) {
    int n;
    session* s = atomic_load(&shown);
    if (wifi_screen(e) || (e->kind == UI_SET && e->setting == NULL)) return;
    if (e->kind == UI_ACTION && strcmp(e->name, "open") == 0) {
        const ssh_host* h = state.hosts;
        for (size_t i = 0; h != NULL && i < e->index; i++)
            h = h->next;
        if (h == NULL) return;
        // At boot the first session's task waits for the choice
        if (atomic_load(&s->host) == NULL)
            atomic_store(&wanted_host, h);
        else if ((s = open_session(0, h, "")) != NULL) {
            storage_set_session(number(s), h->alias, "");
            storage_set_last_host(h->alias);
            show(s);
        } else
            ESP_LOGW(TAG, "no session opened to %s: %d open at most, or no memory", h->alias, MAX_SESSIONS);
        screen_open(NULL);
        return;
    }
    // The list only chooses the session that the buttons for it act on
    if (e->kind == UI_ACTION && strcmp(e->name, "session select") == 0) return;
    if (e->kind == UI_ACTION && strcmp(e->name, "session show") == 0) {
        if ((s = open_at(e->index)) != NULL) show(s);
        screen_open(NULL);
        return;
    }
    if (e->kind == UI_ACTION && strcmp(e->name, "session end") == 0) {
        if ((s = open_at(e->index)) != NULL) end_session(s);
        return;
    }
    // A session's tab: the shown one's opens the sessions screen
    if (e->kind == UI_ACTION && sscanf(e->name, "session %d", &n) == 1) {
        if (n < 1 || n > MAX_SESSIONS || !is_open(&sessions[n - 1])) return;
        if (is_shown(&sessions[n - 1]))
            screen_open("sessions");
        else
            show(&sessions[n - 1]);
        return;
    }
    if (e->kind == UI_ACTION && strcmp(e->name, "reboot") == 0) {
        ESP_LOGI(TAG, "restart from the screen");
        vTaskDelay(pdMS_TO_TICKS(500)); // for the log writer
        esp_restart();
    }
    if (e->kind == UI_ACTION) {
        ESP_LOGI(TAG, "%s: %s", e->name, command(e->name) ? "done" : "unknown");
        return;
    }
    change(e->setting, &e->value);
}

// JSON trees, such as the layout the screen keeps, are hundreds of small blocks: in internal RAM they would take the room the
// C6's SDIO buffers need
static void* psram_malloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

static void ran(void* arg) {
    (void)arg;
    update_ran();
}

// Kept once it has run 10 s: an image that crashes while starting rolls back at the next restart
static void keep_image_later(void) {
    esp_timer_handle_t keep;
    if (esp_timer_create(&(esp_timer_create_args_t){.callback = ran, .name = "keep image"}, &keep) != ESP_OK ||
        esp_timer_start_once(keep, 10 * 1000000) != ESP_OK)
        update_ran();
}

void app_main(void) {
    cJSON_InitHooks(&(cJSON_Hooks){psram_malloc, free});
    memory_log_start();
    if (logs_start() != ESP_OK) ESP_LOGE(TAG, "no log ring");
    if (power_start() != ESP_OK) ESP_LOGE(TAG, "no power for the C6, the USB-A port or charging");
    ESP_ERROR_CHECK(keys_start());
    esp_err_t keyboard = keyboard_usb_start();
    ESP_ERROR_CHECK(update_start());
    ESP_ERROR_CHECK(usb_start(command));
    if (keyboard != ESP_OK) ESP_LOGE(TAG, "no USB keyboard: %s", esp_err_to_name(keyboard));
    if (keyboard_tab5_start() != ESP_OK) ESP_LOGE(TAG, "no Tab5 keyboard");
    ESP_ERROR_CHECK(nvs_flash_init());
    if (storage_mount(usb_storage_boot()) != 0) ESP_LOGI(TAG, "no FAT SD card");
    if (usb_storage_boot()) return;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (clock_start() != ESP_OK) ESP_LOGE(TAG, "no SNTP");
    if (catm_start() != ESP_OK) ESP_LOGE(TAG, "no PPP interface");
    load_config();
    // Before the screen, which takes 2.7 s to start: the C6 restarts and Wi-Fi joins meanwhile. Also without a host, for the
    // control commands
    if (net_start(&state) != ESP_OK) ESP_LOGE(TAG, "no network");
    esp_err_t screen = screen_start(&state);
    if (screen == ESP_OK) screen_resized(&state.terminal_size);
    keep_image_later();
    if (remote_start(&state, command) != ESP_OK) ESP_LOGE(TAG, "no remote control");
    saves = xQueueCreate(4, sizeof(save));
    input_lock = xSemaphoreCreateMutex();
    // Held for good: the sessions' own Init and Cleanup then never tear wolfCrypt down under one another
    if (wolfSSH_Init() != WS_SUCCESS) ESP_LOGE(TAG, "no wolfSSH");
    names_lock = xSemaphoreCreateMutex();
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(tmux_prefix, sizeof tmux_prefix, "tab5-%02x%02x%02x-", mac[3], mac[4], mac[5]);
    session* first = saves != NULL && input_lock != NULL && names_lock != NULL ? restore_sessions() : NULL;
    if (first == NULL && saves != NULL && input_lock != NULL && names_lock != NULL) first = open_session(1, NULL, "");
    ESP_ERROR_CHECK(first != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    show(first);
    if (screen != ESP_OK) ESP_LOGE(TAG, "no screen");
    for (;;) {
        ui_event e;
        terminal_size size;
        save s;
        if (atomic_load(&join.step) == JOIN_DONE) joined_wifi();
        // Not for a session asked to end, whose record is gone
        while (xQueueReceive(saves, &s, 0))
            if (s.number == 0)
                storage_set_last_host(s.host->alias);
            else if (sessions[s.number - 1].id == s.id && is_open(&sessions[s.number - 1]))
                storage_set_session(s.number, s.host->alias, s.tmux);
        tend_sessions();
        memory_log();
        if (screen == ESP_OK) {
            screen_draw();
            // What was typed there goes once it closes
            const char* panel = ui_panel();
            if ((panel == NULL || strcmp(panel, "wifi") != 0) && (state.wifi_edit.network.ssid[0] != 0 || state.wifi_edit.saved[0] != 0))
                memset(&state.wifi_edit, 0, sizeof state.wifi_edit);
            while (screen_event(&e))
                apply(&e);
            memset(&e, 0, sizeof e);
            if (screen_resized(&size)) {
                state.terminal_size = size;
                for (size_t i = 0; i < MAX_SESSIONS; i++)
                    if (sessions[i].used) terminal_resize(sessions[i].terminal, &size);
                atomic_fetch_add(&resizes, 1);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
