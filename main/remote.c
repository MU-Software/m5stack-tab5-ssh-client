// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/stat.h>

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_rom_crc.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <wolfssl/wolfcrypt/memory.h>

#include "c6.h"
#include "c6_nvs.h"
#include "c6_nvs_server.h"
#include "c6_protocol.h"
#include "catm.h"
#include "config.h"
#include "control.h"
#include "logs.h"
#include "settings.h"
#include "sshd.h"
#include "storage.h"
#include "update.h"
#include "wifi.h"

#define PORT            22
#define AUTHORIZED_KEYS SD "/.ssh/authorized_keys"
// The device's own, as /etc/ssh keeps it, beside its configuration; .ssh/ is the client's, as ~/.ssh
#define HOST_KEY   SD "/.config/tab5-ssh/ssh_host_ed25519_key"
#define WAIT_MS    10000
#define CONFIG_MAX (16 * 1024)

static const char* TAG = "remote";
static const global_state* state;
static WOLFSSH_CTX* ctx;
static bool (*app_command)(const char* line);
static atomic_bool busy;
// The client config set answers, and whether a value was refused
static sshd_session* checking;
static bool refusals;

static int write_file(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    if (f == NULL) return -1;
    int err = fputs(text, f) < 0;
    return fclose(f) != 0 || err ? -1 : 0;
}

// Made when there is none, never over one that does not load; its .pub too, for known_hosts before the first connection
static WOLFSSH_CTX* load_host_key(void) {
    char pem[256], line[128], fingerprint[64];
    uint8_t public_key[SSHD_PUBLIC_KEY_SIZE];
    struct stat st;
    char* text = storage_read_file_text(HOST_KEY);
    pem[0] = 0;
    if (text != NULL) {
        strlcpy(pem, text, sizeof pem);
        wc_ForceZero(text, strlen(text));
        free(text);
    } else if (stat(HOST_KEY, &st) != 0 && (sshd_new_host_key(pem, sizeof pem) <= 0 || write_file(HOST_KEY, pem) != 0)) {
        pem[0] = 0;
    }
    WOLFSSH_CTX* made = NULL;
    if (pem[0] && sshd_public_key(pem, public_key) == 0 && ssh_fingerprint(fingerprint, sizeof fingerprint, public_key, sizeof public_key) == 0)
        made = sshd_new(pem);
    wc_ForceZero(pem, sizeof pem);
    if (made == NULL) {
        ESP_LOGE(TAG, "no Ed25519 host key in " HOST_KEY " (PKCS#8 PEM), none made");
        return NULL;
    }
    if (stat(HOST_KEY ".pub", &st) != 0 &&
        (public_key_line(line, sizeof line, public_key, sizeof public_key) != 0 || write_file(HOST_KEY ".pub", line) != 0))
        ESP_LOGW(TAG, "no " HOST_KEY ".pub");
    ESP_LOGI(TAG, "host key %s", fingerprint);
    return made;
}

static void say(sshd_session* s, const char* text) {
    sshd_write(s, text, strlen(text), WAIT_MS);
}

static void write_reply(void* context, const char* text, size_t length) {
    sshd_write(context, text, length, WAIT_MS);
}

// stdin up to its end, each wait at most WAIT_MS
static int read_input(void* context, void* buffer, size_t length) {
    for (TickType_t last = xTaskGetTickCount(); xTaskGetTickCount() - last < pdMS_TO_TICKS(WAIT_MS);) {
        int r = sshd_read(context, buffer, length);
        if (r > 0) return r;
        if (r == WS_EOF) return 0;
        if (r < 0) return -1;
        sshd_wait(context, 100);
    }
    return -1;
}

// The log from position on, until the client leaves or, with quiet_ms, until that long passes without a line
static void follow(sshd_session* s, size_t position, int quiet_ms) {
    char chunk[512];
    for (TickType_t last = xTaskGetTickCount(); quiet_ms == 0 || xTaskGetTickCount() - last < pdMS_TO_TICKS(quiet_ms);) {
        size_t n = logs_read(&position, chunk, sizeof chunk);
        if (n > 0) {
            if (sshd_write(s, chunk, n, WAIT_MS) != WS_SUCCESS) return;
            last = xTaskGetTickCount();
            continue;
        }
        // What the client sends is dropped; reading notices it leave
        int r = sshd_read(s, chunk, sizeof chunk);
        if (r < 0 && r != WS_EOF) return;
        if (r <= 0) sshd_wait(s, 100);
    }
}

// size bytes from stdin into PSRAM, checked against their CRC-32; NULL with err otherwise
static uint8_t* receive(sshd_session* s, size_t size, uint32_t crc, esp_err_t* err) {
    uint8_t* image = size > 0 ? heap_caps_malloc(size, MALLOC_CAP_SPIRAM) : NULL;
    *err = ESP_ERR_INVALID_SIZE;
    if (image == NULL) return NULL;
    size_t got = 0;
    for (TickType_t last = xTaskGetTickCount(); got < size && xTaskGetTickCount() - last < pdMS_TO_TICKS(WAIT_MS);) {
        int n = sshd_read(s, image + got, size - got);
        if (n > 0) {
            got += (size_t)n;
            last = xTaskGetTickCount();
        } else if (n == 0) {
            sshd_wait(s, 100);
        } else {
            break;
        }
    }
    *err = got != size ? ESP_ERR_TIMEOUT : esp_rom_crc32_le(0, image, size) != crc ? ESP_ERR_INVALID_CRC : ESP_OK;
    if (*err == ESP_OK) return image;
    free(image);
    return NULL;
}

// The text holds Wi-Fi passwords: never logged, cleared before it is freed
static int print_config(sshd_session* s) {
    config_source source;
    cJSON* config = storage_get_device_config(&source);
    char* text = cJSON_Print(config);
    cJSON_Delete(config);
    if (text == NULL) return 1;
    say(s, text);
    say(s, "\n");
    wc_ForceZero(text, strlen(text));
    free(text);
    ESP_LOGI(TAG, "configuration read from %s", source == CONFIG_SD ? "the SD card" : "NVS");
    return 0;
}

static void refused(const char* name) {
    char text[64];
    snprintf(text, sizeof text, "%s: not a value for it\n", name);
    say(checking, text);
    refusals = true;
}

// From stdin up to its end, checked as at boot; saved only when all of it is valid, for the next start
static const char* check_config(sshd_session* s, cJSON** config) {
    char* text = heap_caps_calloc(1, CONFIG_MAX + 1, MALLOC_CAP_SPIRAM);
    size_t got = 0;
    int r = 0;
    for (TickType_t last = xTaskGetTickCount();
         text != NULL && r != WS_EOF && got < CONFIG_MAX && xTaskGetTickCount() - last < pdMS_TO_TICKS(WAIT_MS);) {
        if ((r = sshd_read(s, text + got, CONFIG_MAX - got)) > 0)
            got += (size_t)r, last = xTaskGetTickCount();
        else if (r == 0)
            sshd_wait(s, 100);
        else if (r != WS_EOF)
            break;
    }
    *config = r == WS_EOF ? cJSON_ParseWithOpts(text, NULL, true) : NULL;
    if (text != NULL) wc_ForceZero(text, CONFIG_MAX);
    free(text);
    if (r != WS_EOF) return got >= CONFIG_MAX ? "16 KiB or more" : "no end of input";
    if (*config == NULL) return "not JSON";
    wifi_network* wifi;
    if (config_wifi_networks(*config, &wifi) != 0) return "not an object, or the Wi-Fi list is invalid";
    if (wifi == NULL) return "no Wi-Fi network: nothing would reach the Tab5 after the restart";
    for (wifi_network* w = wifi; w != NULL; w = w->next)
        wc_ForceZero(w->password, sizeof w->password);
    free(wifi);
    global_state scratch = *state;
    checking = s;
    refusals = false;
    settings_load(&scratch, *config, refused);
    return refusals ? "a setting is invalid" : NULL;
}

static int set_config(sshd_session* s) {
    cJSON* config;
    const char* error = check_config(s, &config);
    if (error != NULL)
        cJSON_Delete(config);
    else if (storage_set_device_config(config) != 0)
        error = "not saved";
    char text[96];
    snprintf(text, sizeof text, "config %s%s\n", error == NULL ? "saved; restart applies it" : "refused: ", error == NULL ? "" : error);
    say(s, text);
    ESP_LOGI(TAG, "config set: %s", error == NULL ? "saved" : error);
    return error == NULL ? 0 : 1;
}

typedef enum { STAY, RESTART, RESTART_UPDATED } after;

// Its exit status. The app's commands answer in the log, which follows them for as long as it has lines
static int run(sshd_session* s, after* then) {
    const char* line = s->command;
    unsigned long size, crc;
    char text[96];
    if (strcmp(line, "log") == 0) {
        follow(s, 0, 0);
        return 0;
    }
    if (strcmp(line, "version") == 0) {
        const esp_app_desc_t* app = esp_app_get_description();
        snprintf(text, sizeof text, "%s %s, %s\n", app->project_name, app->version, esp_ota_get_running_partition()->label);
        say(s, text);
        return 0;
    }
    if (strcmp(line, "config") == 0) return print_config(s);
    if (strcmp(line, "config set") == 0) return set_config(s);
    // Values go to stdout alone, never to the log
    if (control_nvs(line) > 0) {
        bool c6 = line[0] == 'c';
        c6_nvs_io io = {.name = c6 ? "c6 nvs" : "nvs",
                        .protocol = c6 ? c6_protocol() : C6_PROTOCOL,
                        .exchange = c6 ? c6_nvs : c6_nvs_local,
                        .error_name = esp_err_to_name,
                        .write = write_reply,
                        .read = read_input,
                        .context = s};
        return c6_nvs_command(line + control_nvs(line), &io);
    }
    if (strcmp(line, "restart") == 0) {
        *then = RESTART;
        return 0;
    }
    if (sscanf(line, "update %lu %lx", &size, &crc) == 2 || sscanf(line, "c6 update %lu %lx", &size, &crc) == 2) {
        bool c6 = line[0] == 'c';
        esp_err_t err;
        uint8_t* image = receive(s, size, (uint32_t)crc, &err);
        if (image != NULL) err = update_install(c6, image, size);
        free(image);
        snprintf(text, sizeof text, "%supdate %s\n", c6 ? "c6 " : "", esp_err_to_name(err));
        say(s, text);
        if (err == ESP_OK) *then = RESTART_UPDATED;
        return err == ESP_OK ? 0 : 1;
    }
    size_t position = logs_position();
    if (!line[0] || !app_command(line)) {
        char help[400];
        control_help(CONTROL_SSH, help, sizeof help);
        say(s, help);
        say(s, "\n");
        return 1;
    }
    follow(s, position, 1000);
    return 0;
}

// Wi-Fi, and LTE once remote.lte is on: behind CGNAT nobody reaches it there, and whatever does costs data
static bool allowed(uint32_t local) {
    esp_netif_ip_info_t ip;
    return (esp_netif_get_ip_info(wifi_netif(), &ip) == ESP_OK && ip.ip.addr == local) ||
           (state->remote.lte && esp_netif_get_ip_info(catm_netif(), &ip) == ESP_OK && ip.ip.addr == local);
}

static void serve(void* arg) {
    int fd = (int)(intptr_t)arg;
    struct sockaddr_in peer = {0}, local = {0};
    socklen_t length = sizeof peer;
    char from[16] = "?";
    if (getpeername(fd, (struct sockaddr*)&peer, &length) == 0) inet_ntoa_r(peer.sin_addr, from, sizeof from);
    length = sizeof local;
    if (getsockname(fd, (struct sockaddr*)&local, &length) != 0 || !allowed(local.sin_addr.s_addr)) {
        ESP_LOGW(TAG, "%s: not over Wi-Fi%s", from, state->remote.lte ? " or LTE" : "");
        close(fd);
    } else {
        // Read for each client: a key taken off the list is refused from then on
        sshd_session s = {.authorized_keys = storage_read_file_text(AUTHORIZED_KEYS), .user = state->remote.user};
        int rc = sshd_accept(ctx, &s, fd, WAIT_MS);
        free((char*)s.authorized_keys);
        if (rc != WS_SUCCESS) {
            if (s.refused)
                ESP_LOGW(TAG, "%s: refused, key %s", from, s.fingerprint);
            else
                ESP_LOGW(TAG, "%s: %s", from, wolfSSH_ErrorToName(rc));
        } else {
            ESP_LOGI(TAG, "%s: %s runs \"%.*s\"", from, s.fingerprint, control_loggable(s.command), s.command);
            update_works(UPDATE_NETWORK);
            after then = STAY;
            sshd_close(&s, run(&s, &then));
            ESP_LOGI(TAG, "%s: done, %u bytes of stack never used", from, (unsigned)uxTaskGetStackHighWaterMark(NULL));
            if (then == RESTART_UPDATED) update_restart(UPDATE_NETWORK);
            if (then == RESTART) esp_restart();
        }
    }
    atomic_store(&busy, false);
    vTaskDelete(NULL);
}

static int open_port(void) {
    struct stat st;
    if (stat(AUTHORIZED_KEYS, &st) != 0) {
        ESP_LOGW(TAG, "no " AUTHORIZED_KEYS ": port %d stays closed", PORT);
        return -1;
    }
    if (ctx == NULL && (ctx = load_host_key()) == NULL) return -1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0 && (bind(fd, (struct sockaddr*)&address, sizeof address) != 0 || listen(fd, 1) != 0)) {
        close(fd);
        return -1;
    }
    if (fd >= 0) ESP_LOGI(TAG, "listening on port %d", PORT);
    return fd;
}

// One client at a time, another closed at once, served on an internal stack (writing flash needs one) only while it is there
static void listen_for_clients(void* arg) {
    (void)arg;
    int listener = -1, fd;
    bool tried = false;
    for (;;) {
        // remote.enabled opens and closes the port, looked at each second; turning it on again tries again
        if (!state->remote.enabled) {
            tried = false;
            if (listener >= 0) {
                close(listener);
                listener = -1;
                ESP_LOGI(TAG, "port %d closed", PORT);
            }
        } else if (listener < 0 && !tried) {
            tried = true;
            listener = open_port();
        }
        if (listener < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (poll(&(struct pollfd){.fd = listener, .events = POLLIN}, 1, 1000) != 1) continue;
        if ((fd = accept(listener, NULL, NULL)) < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else if (atomic_exchange(&busy, true)) {
            close(fd);
        } else if (xTaskCreatePinnedToCore(serve, "remote", 16 * 1024, (void*)(intptr_t)fd, 5, NULL, 1) != pdPASS) {
            ESP_LOGW(TAG, "no room for a client");
            close(fd);
            atomic_store(&busy, false);
        }
    }
}

esp_err_t remote_start(const global_state* global, bool (*command)(const char* line)) {
    state = global;
    app_command = command;
    // A PSRAM stack: it writes the SD card, never flash
    return xTaskCreatePinnedToCoreWithCaps(listen_for_clients, "remote listen", 8192, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM) == pdPASS ? ESP_OK
                                                                                                                                     : ESP_ERR_NO_MEM;
}
