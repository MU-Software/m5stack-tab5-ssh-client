// SPDX-License-Identifier: GPL-3.0-or-later
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
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
#include "net.h"
#include "power.h"
#include "screen.h"
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
};

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

typedef enum { SESSION_CLOSED, SESSION_REFUSED, SESSION_DROPPED, SESSION_MOVED } session_end;

// Typing that 1 s brings no output pings the host; no answer for another second shows as held until anything arrives
static session_end bridge(unsigned path) {
    uint8_t in[256], out[1024];
    int pending = 0, off = 0;
    TickType_t typed_at = 0, pinged_at = 0;
    bool typed = false, pinged = false, held = false;
    unsigned pongs = 0;
    for (;;) {
        if (net_generation() != path) return SESSION_MOVED;
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
        ESP_LOGI(TAG, "session %s, exit status %d", (const char* const[]){"closed", "refused", "dropped", "moved"}[end], *exit_status);
    } else {
        ESP_LOGE(TAG, "ssh %s@%s:%u: %s", h->user, h->hostname, h->port, wolfSSH_ErrorToName(rc));
    }
    state.session = NULL;
    return end;
}

// Attaches to the host's tmux session kept in NVS, or starts one when there is none or it has gone
static session_end tmux(const ssh_host* h, unsigned path) {
    char name[16], command[400];
    bool attach = storage_get_tmux_session(h->alias, name, sizeof name) == ESP_OK;
    for (;;) {
        if (!attach) {
            snprintf(name, sizeof name, "tab5-%08" PRIx32, esp_random());
            storage_set_tmux_session(h->alias, name);
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
            storage_set_tmux_session(h->alias, NULL);
            message("\r\n\x1b[33mNo tmux on this host: a plain shell, lost when the connection drops\x1b[m\r\n");
            return run(h, NULL, path, &exit_status);
        }
        if (!attach || exit_status == 0) return end;
        storage_set_tmux_session(h->alias, NULL);
        attach = false;
    }
}

static void wait_enter(void) {
    for (;;) {
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

// Reconnects after a drop or a change of path at once (tmux keeps the screen), after a closed session on Enter
static void sessions(const ssh_host* h) {
    int retry_ms = 1000;
    for (;;) {
        session_end end = tmux(h, wait_path());
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

static void connect(void) {
    static const char* const sources[] = {"nothing", "the SD card", "NVS"};
    config_source source;
    cJSON* config = storage_get_device_config(&source);
    if (config_wifi_networks(config, &state.wifi) != 0)
        ESP_LOGE(TAG, "the configuration from %s is invalid", sources[source]);
    else
        ESP_LOGI(TAG, "configuration from %s", sources[source]);
    if (config != NULL && config_log(config, &state.log, state.timezone, sizeof state.timezone) != 0)
        ESP_LOGE(TAG, "the log settings or the time zone from %s are invalid", sources[source]);
    cJSON_Delete(config);
    clock_timezone(state.timezone);
    logs_configure(&state.log);
    char* text = storage_get_ssh_config();
    state.hosts = text != NULL ? config_ssh_hosts(text) : NULL;
    free(text);
    state.current_host = state.hosts;
    state.wifi_off = storage_get_wifi_off();
    // Also without a host, for the control commands
    if (net_start(&state) != ESP_OK) ESP_LOGE(TAG, "no network");
    const ssh_host* h = state.current_host;
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
    if (state.wifi_off) storage_set_wifi_off(state.wifi_off = false);
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

static bool command(const char* line) {
    unsigned long size, crc;
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
    // wifi off alone lasts the boot
    if (strcmp(line, "wifi off save") == 0 || strcmp(line, "wifi on") == 0) {
        state.wifi_off = strcmp(line, "wifi on") != 0;
        storage_set_wifi_off(state.wifi_off);
        return net_command(state.wifi_off ? "wifi off" : "wifi on");
    }
    if (strncmp(line, "wifi forget ", 12) == 0) {
        // Emptied, not unlinked, for the same reason
        for (wifi_network* w = state.wifi; w != NULL; w = w->next)
            if (strcmp(w->ssid, line + 12) == 0) w->ssid[0] = 0;
        ESP_LOGI(TAG, "wifi forget %s: %s", line + 12, storage_add_wifi(line + 12, NULL) == 0 ? "done" : "not saved");
        return true;
    }
    return net_command(line);
}

static void run_session(void* arg) {
    (void)arg;
    connect();
    vTaskDelete(NULL);
}

void app_main(void) {
    if (logs_start() != ESP_OK) ESP_LOGE(TAG, "no log ring");
    if (power_start() != ESP_OK) ESP_LOGE(TAG, "charging left as it was");
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
    ESP_ERROR_CHECK(terminal_start(&state.terminal_size));
    esp_err_t screen = screen_start(&state);
    usb_keep_image();
    // Ghostty's measured stack is 27-33 KiB
    xTaskCreatePinnedToCore(run_session, "session", 48 * 1024, NULL, 5, NULL, 1);
    if (screen != ESP_OK) {
        ESP_LOGE(TAG, "no screen");
        return;
    }
    for (;;) {
        screen_draw();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
