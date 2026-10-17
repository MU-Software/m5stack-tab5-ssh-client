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
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <nvs_flash.h>
#include <wolfssl/wolfcrypt/memory.h>

#include "c6.h"
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
#include "screen.h"
#include "settings.h"
#include "ssh.h"
#include "storage.h"
#include "terminal.h"
#include "usb.h"

static const char* TAG = "main";
static ssh_session session;
static global_state state = {
    .terminal_type = "xterm-256color",
    .terminal_size = {.columns = 106, .rows = 30, .width_pixels = 1272, .height_pixels = 720},
    .log = {.enabled = true, .file_kb = 1024, .keep_mb = 32, .keep_days = 14},
    .timezone = "KST-9",
    .echo = {true, 50},
    .bars = {true, true},
};

// Bumped by each change of the terminal's size; the host chosen on the screen, NULL when none waits
static atomic_uint resizes;
static _Atomic(const ssh_host*) wanted_host;

static void message(const char* text) {
    terminal_write(text, strlen(text));
    usb_terminal_write(text, strlen(text));
}

// From the keyboards or the USB terminal port, on the screen and the port
static void read_line(const char* prompt, char* buf, size_t size, int echo) {
    message(prompt);
    size_t n = 0;
    for (;;) {
        char c;
        if (keys_read(&c, 1) != 1 && usb_terminal_read(&c, 1) != 1) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == '\r' || c == '\n') break;
        if (c == 0x7f || c == '\b') {
            if (n == 0) continue;
            n--;
            if (echo) message("\b \b");
        } else if (n + 1 < size) {
            buf[n++] = c;
            if (echo) message((char[]){c, 0});
        }
    }
    buf[n] = 0;
    message("\r\n");
}

int accept_new_host(const global_state* state, const char* name, const char* fingerprint, const char* line) {
    (void)state;
    char prompt[512], answer[8];
    snprintf(prompt, sizeof prompt,
             "The authenticity of host '%s' can't be established.\r\nKey fingerprint is %s.\r\n"
             "Are you sure you want to continue connecting (yes/no)? ",
             name, fingerprint);
    read_line(prompt, answer, sizeof answer, 1);
    if (strcmp(answer, "yes") != 0) return 0;
    if (storage_add_known_host(line) != 0) ESP_LOGW(TAG, "could not add %s to " SD "/.ssh/known_hosts", name);
    return 1;
}

typedef enum { SESSION_CLOSED, SESSION_REFUSED, SESSION_DROPPED, SESSION_MOVED, SESSION_SWITCHED } session_end;

// Typing that 1 s brings no output pings the host; no answer for another second shows as held until anything arrives
static session_end bridge(unsigned path) {
    uint8_t in[256], out[1024];
    int pending = 0, off = 0;
    TickType_t typed_at = 0, pinged_at = 0;
    bool typed = false, pinged = false, held = false;
    unsigned pongs = 0, size = atomic_load(&resizes);
    for (;;) {
        if (net_generation() != path) return SESSION_MOVED;
        if (atomic_load(&wanted_host) != NULL) return SESSION_SWITCHED;
        if (atomic_load(&resizes) != size) size = atomic_load(&resizes), ssh_resize(&state);
        if (pending == 0) {
            if ((pending = (int)terminal_reply(in, sizeof in)) == 0) {
                if ((pending = (int)keys_read((char*)in, sizeof in)) == 0) pending = (int)usb_terminal_read(in, sizeof in);
                if (pending > 0) {
                    screen_typed(in, (size_t)pending);
                    if (!typed) typed = true, typed_at = xTaskGetTickCount();
                }
            }
            off = 0;
        }
        int sent = pending > 0 ? ssh_write(&state, in + off, (size_t)pending) : WS_WANT_WRITE;
        int got = ssh_read(&state, out, sizeof out);
        if (got == WS_CHANNEL_CLOSED) return SESSION_CLOSED;
        if ((sent < 0 && sent != WS_WANT_WRITE) || (got < 0 && got != WS_WANT_READ)) return SESSION_DROPPED;
        if (sent > 0) {
            off += sent;
            pending -= sent;
        }
        TickType_t now = xTaskGetTickCount();
        if (got > 0 || (pinged && session.pongs != pongs)) {
            typed = pinged = false;
            if (held) screen_waiting(NULL, 0), held = false;
        } else if (typed && !pinged && now - typed_at >= pdMS_TO_TICKS(1000)) {
            pongs = session.pongs, pinged = true, pinged_at = now;
            ssh_ping(&state);
        } else if (pinged && !held && now - pinged_at >= pdMS_TO_TICKS(1000)) {
            screen_waiting("held", typed_at);
            held = true;
        }
        if (got > 0) {
            terminal_write(out, (size_t)got);
            usb_terminal_write(out, (size_t)got);
        } else if (sent < 0)
            ssh_wait(&state, 10);
    }
}

// The exit status counts only for SESSION_CLOSED
static session_end run(const ssh_host* h, const char* command, unsigned path, int* exit_status) {
    char* private_key = storage_get_private_key(h->identity_file);
    char password[128] = "";
    if (private_key == NULL) {
        char prompt[300];
        snprintf(prompt, sizeof prompt, "%s@%s's password: ", h->user, h->hostname);
        read_line(prompt, password, sizeof password, 0);
    }
    char* known_hosts = storage_get_known_hosts();
    session = (ssh_session){.known_hosts = known_hosts,
                            .private_key = private_key,
                            .password = password[0] ? password : NULL,
                            .command = command,
                            .local_address = net_local_address(),
                            .timeout_ms = net_on_lte() ? 30000 : 10000};
    state.session = &session;
    int rc = ssh_open(&state);
    if (private_key != NULL) wc_ForceZero(private_key, strlen(private_key));
    free(private_key);
    free(known_hosts);
    memset(password, 0, sizeof password);
    // Refused: retrying the same keys and host key would only be refused again
    session_end end = rc == WS_SOCKET_ERROR_E ? SESSION_DROPPED : SESSION_REFUSED;
    *exit_status = 0;
    if (rc == WS_SUCCESS) {
        net_keepalive(session.fd);
        screen_waiting(NULL, 0);
        ESP_LOGI(TAG, "connected to %s: %s", h->hostname, command != NULL ? command : "shell");
        if (state.log.quiet_sessions) {
            esp_log_level_set("*", ESP_LOG_WARN);
            // They answer control commands
            for (size_t i = 0; i < 5; i++)
                esp_log_level_set((const char* const[]){"catm", "main", "net", "power", "wifi"}[i], ESP_LOG_INFO);
        }
        end = bridge(path);
        screen_typed(NULL, 0);
        esp_log_level_set("*", ESP_LOG_INFO);
        ssh_close(&state, exit_status);
        ESP_LOGI(TAG, "session %s, exit status %d", (const char* const[]){"closed", "refused", "dropped", "moved", "switched"}[end], *exit_status);
    } else {
        ESP_LOGE(TAG, "ssh %s@%s:%u: %s", h->user, h->hostname, h->port, wolfSSH_ErrorToName(rc));
    }
    state.session = NULL;
    return end;
}

// NVS writes from the session task, whose stack is in PSRAM: writing flash needs an internal stack, main's loop does them
typedef struct {
    const ssh_host* host;
    bool tmux;
    char name[16];
} save;
static QueueHandle_t saves;

static void save_tmux(const ssh_host* h, const char* name) {
    save s = {h, true, ""};
    if (name != NULL) snprintf(s.name, sizeof s.name, "%s", name);
    xQueueSend(saves, &s, portMAX_DELAY);
}

static void save_last_host(const ssh_host* h) {
    save s = {h, false, ""};
    xQueueSend(saves, &s, portMAX_DELAY);
}

// Attaches to the host's tmux session kept in NVS, or starts one when there is none or it has gone
static session_end tmux(const ssh_host* h, unsigned path) {
    char name[16], command[400];
    bool attach = storage_get_tmux_session(h->alias, name, sizeof name) == ESP_OK;
    for (;;) {
        if (!attach) {
            snprintf(name, sizeof name, "tab5-%08" PRIx32, esp_random());
            save_tmux(h, name);
        }
        // -T (tmux 3.2) declares what the screen draws beyond xterm-256color's terminfo. allow-passthrough (tmux 3.3) lets
        // kitty images reach the screen, for this session's windows only. '=': an exact name, not a prefix (set-hook takes none).
        snprintf(command, sizeof command,
                 "sh -c 'f=-TRGB,strikethrough,overline,usstyle,kittygraphics; tmux $f -V >/dev/null 2>&1 || f=; s=%s; %s"
                 "tmux set-hook -t $s after-new-window \"set -w allow-passthrough all\" 2>/dev/null; "
                 "for w in $(tmux list-windows -t =$s -F \"#{window_id}\" 2>/dev/null); do tmux set -w -t $w allow-passthrough all; done; "
                 "exec tmux $f attach-session -t =$s'",
                 name, attach ? "" : "tmux new-session -d -s $s || exit $?; ");
        int exit_status;
        session_end end = run(h, command, path, &exit_status);
        if (end != SESSION_CLOSED) return end;
        if (exit_status == 127) {
            save_tmux(h, NULL);
            message("\r\n\x1b[33mNo tmux on this host: a plain shell, lost when the connection drops\x1b[m\r\n");
            return run(h, NULL, path, &exit_status);
        }
        if (!attach || exit_status == 0) return end;
        save_tmux(h, NULL);
        attach = false;
    }
}

// Or until another host is chosen
static void wait_enter(void) {
    while (atomic_load(&wanted_host) == NULL) {
        char in[64];
        size_t n = keys_read(in, sizeof in);
        if (n == 0) n = usb_terminal_read(in, sizeof in);
        if (memchr(in, '\r', n) != NULL) return;
        if (n == 0) vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Until there is a path, what LTE is doing; Enter tries LTE at once, the other keys are dropped
static unsigned wait_path(void) {
    static const char* const states[] = {
        [NET_LTE_OFF] = "no network: LTE off, Enter turns it on",
        [NET_LTE_NO_MODEM] = "no network: no LTE modem",
        [NET_LTE_DIALING] = "no network: LTE dialing",
        [NET_LTE_REGISTERING] = "no network: LTE not registered",
        [NET_LTE_WAITING] = "no network: LTE waits, Enter retries",
        [NET_LTE_UP] = "no network",
    };
    unsigned path;
    TickType_t since = xTaskGetTickCount();
    while (!net_wait_for(200, &path)) {
        char in[64];
        size_t n = keys_read(in, sizeof in);
        if (n == 0) n = usb_terminal_read(in, sizeof in);
        if (memchr(in, '\r', n) != NULL) net_command("lte on");
        screen_waiting(states[net_lte()], since);
    }
    return path;
}

// Reconnects after a drop or a change of path at once (tmux keeps the screen), after a closed session on Enter; another
// host chosen on the screen ends the session
static void sessions(const ssh_host* h) {
    int retry_ms = 1000;
    for (;;) {
        const ssh_host* chosen = atomic_exchange(&wanted_host, NULL);
        if (chosen != NULL) {
            h = state.current_host = chosen;
            save_last_host(h);
            message("\x1b[2J\x1b[H");
        }
        session_end end = tmux(h, wait_path());
        if (end == SESSION_SWITCHED) continue;
        screen_waiting(end == SESSION_DROPPED || end == SESSION_MOVED ? "reconnecting" : NULL, xTaskGetTickCount());
        if (end == SESSION_CLOSED || end == SESSION_REFUSED) {
            message(end == SESSION_CLOSED ? "\r\n[Session closed. Enter connects again.]\r\n" : "\r\n[Not connected. Enter tries again.]\r\n");
            wait_enter();
        } else if (end == SESSION_DROPPED) {
            net_suspect();
            // A new path is worth trying at once
            for (unsigned path = net_generation(), waited = 0; waited < retry_ms && net_generation() == path; waited += 100)
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
    screen_open("hosts");
    while (screen_panel_open() && atomic_load(&wanted_host) == NULL) {
        if (state.host_deadline_us != 0 && screen_input()) state.host_deadline_us = 0;
        if (state.host_deadline_us != 0 && esp_timer_get_time() >= state.host_deadline_us) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    state.host_deadline_us = 0;
    const ssh_host* chosen = atomic_exchange(&wanted_host, NULL);
    if (chosen != NULL) state.current_host = chosen;
    if (state.current_host != NULL) save_last_host(state.current_host);
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

static void connect(void) {
    ESP_LOGI(TAG, "internal DMA RAM: %u free, %u in one block", heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    const ssh_host* h = choose_host();
    if (h == NULL || !h->user[0]) {
        ESP_LOGE(TAG, "incomplete configuration: a Host with a User in the SD card's .ssh/config");
        return;
    }
    sessions(h);
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

// The C6 boots the new image once the restart powers it again; it rolls back if that one does not stay up
static void c6_update_command(size_t size, uint32_t crc) {
    esp_err_t err;
    uint8_t* image = usb_receive(size, crc, &err);
    if (image != NULL) err = c6_update(image, size);
    free(image);
    char reply[48];
    snprintf(reply, sizeof reply, "c6 update %s\n", esp_err_to_name(err));
    usb_reply(reply);
    if (err != ESP_OK) return;
    vTaskDelay(pdMS_TO_TICKS(200)); // for the host to read the reply
    esp_restart();
}

static void log_modem(void) {
    modem_snapshot* s = heap_caps_malloc(sizeof *s, MALLOC_CAP_SPIRAM);
    if (s == NULL) return;
    catm_snapshot(s);
    int64_t now = esp_timer_get_time();
    if (catm_connected()) ESP_LOGI(TAG, "modem: PPP is up, values from before the dial (lte off to read again)");
    for (int i = 0; i < MODEM_ITEMS; i++)
        if (s->read_at[i] != 0) ESP_LOGI(TAG, "modem %s: %s (%lld s ago)", modem_commands[i].name, s->values[i], (now - s->read_at[i]) / 1000000);
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
    unsigned long size, crc;
    if (strncmp(line, "modem ", 6) == 0) {
        modem_control(line + 6);
        return true;
    }
    if (sscanf(line, "c6 update %lu %lx", &size, &crc) == 2) {
        c6_update_command(size, (uint32_t)crc);
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

// From the screen's task, whose stack in internal RAM lets it write NVS
static void apply(const ui_event* e) {
    char text[80];
    if (wifi_screen(e) || (e->kind == UI_SET && e->setting == NULL)) return;
    if (e->kind == UI_ACTION && strcmp(e->name, "connect") == 0) {
        const ssh_host* h = state.hosts;
        for (size_t i = 0; h != NULL && i < e->index; i++)
            h = h->next;
        if (h != NULL && h != state.current_host) atomic_store(&wanted_host, h);
        if (h != NULL) screen_open(NULL);
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
    settings_text(e->setting, &e->value, text, sizeof text);
    if (!settings_set(&state, e->setting, &e->value)) {
        ESP_LOGW(TAG, "%s %s refused", e->name, text);
        return;
    }
    bool saved = storage_set_config(e->name, settings_json(&state, e->setting)) == 0;
    ESP_LOGI(TAG, "%s %s%s", e->name, text, saved ? "" : ", not saved");
    if (strncmp(e->name, "log.", 4) == 0) logs_configure(&state.log);
    if (strcmp(e->name, "timezone") == 0) clock_timezone(state.timezone);
}

// JSON trees, such as the layout the screen keeps, are hundreds of small blocks: in internal RAM they would take the room the
// C6's SDIO buffers need
static void* psram_malloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

static void keep_image(void* arg) {
    (void)arg;
    usb_keep_image();
}

// Kept once it has run 10 s: an image that crashes while starting rolls back at the next restart
static void keep_image_later(void) {
    esp_timer_handle_t keep;
    if (esp_timer_create(&(esp_timer_create_args_t){.callback = keep_image, .name = "keep image"}, &keep) != ESP_OK ||
        esp_timer_start_once(keep, 10 * 1000000) != ESP_OK)
        usb_keep_image();
}

static void run_session(void* arg) {
    (void)arg;
    connect();
    vTaskDeleteWithCaps(NULL);
}

void app_main(void) {
    cJSON_InitHooks(&(cJSON_Hooks){psram_malloc, free});
    memory_log_start();
    if (logs_start() != ESP_OK) ESP_LOGE(TAG, "no log ring");
    if (power_start() != ESP_OK) ESP_LOGE(TAG, "no power for the C6, the USB-A port or charging");
    ESP_ERROR_CHECK(keys_start());
    esp_err_t keyboard = keyboard_usb_start();
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
    ESP_ERROR_CHECK(terminal_start(&state.terminal_size));
    keep_image_later();
    saves = xQueueCreate(4, sizeof(save));
    // Ghostty's measured stack is 27-33 KiB, as fast in PSRAM; flash writes go through saves
    if (saves == NULL || xTaskCreatePinnedToCoreWithCaps(run_session, "session", 48 * 1024, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS)
        ESP_LOGE(TAG, "no session");
    if (screen != ESP_OK) ESP_LOGE(TAG, "no screen");
    for (;;) {
        ui_event e;
        terminal_size size;
        save s;
        if (atomic_load(&join.step) == JOIN_DONE) joined_wifi();
        while (saves != NULL && xQueueReceive(saves, &s, 0))
            s.tmux ? storage_set_tmux_session(s.host->alias, s.name[0] != 0 ? s.name : NULL) : storage_set_last_host(s.host->alias);
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
                atomic_fetch_add(&resizes, 1);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
