// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs_flash.h>
#include <wolfssl/wolfcrypt/memory.h>

#include "catm.h"
#include "config.h"
#include "keyboard_tab5.h"
#include "keyboard_usb.h"
#include "keys.h"
#include "net.h"
#include "screen.h"
#include "ssh.h"
#include "storage.h"
#include "terminal.h"
#include "usb.h"
#include "wifi.h"

static const char* TAG = "main";
static ssh_session session;
static SemaphoreHandle_t online;
static global_state state = {
    .terminal_type = "xterm-256color",
    .terminal_size = {.columns = 106, .rows = 30, .width_pixels = 1272, .height_pixels = 720},
};

static void read_line(const char* prompt, char* buf, size_t size, int echo) {
    usb_terminal_write(prompt, strlen(prompt));
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (usb_terminal_read(&c, 1) != 1) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == '\r' || c == '\n') break;
        if (c == 0x7f || c == '\b') {
            if (n == 0) continue;
            n--;
            if (echo) usb_terminal_write("\b \b", 3);
        } else if (n + 1 < size) {
            buf[n++] = (char)c;
            if (echo) usb_terminal_write(&c, 1);
        }
    }
    buf[n] = 0;
    usb_terminal_write("\r\n", 2);
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

static void bridge(void) {
    uint8_t in[256], out[1024];
    int pending = 0, off = 0;
    for (;;) {
        if (pending == 0) {
            if ((pending = (int)terminal_reply(in, sizeof in)) == 0 && (pending = (int)keys_read((char*)in, sizeof in)) == 0)
                pending = (int)usb_terminal_read(in, sizeof in);
            off = 0;
        }
        int sent = pending > 0 ? ssh_write(&state, in + off, (size_t)pending) : WS_WANT_WRITE;
        int got = ssh_read(&state, out, sizeof out);
        if ((sent < 0 && sent != WS_WANT_WRITE) || (got < 0 && got != WS_WANT_READ)) return;
        if (sent > 0) {
            off += sent;
            pending -= sent;
        }
        if (got > 0) {
            terminal_write(out, (size_t)got);
            usb_terminal_write(out, (size_t)got);
        } else if (sent < 0)
            ssh_wait(&state, 10);
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
    cJSON_Delete(config);
    char* text = storage_get_ssh_config();
    state.hosts = text != NULL ? config_ssh_hosts(text) : NULL;
    free(text);
    state.current_host = state.hosts;
    ssh_host* h = state.current_host;
    if (h == NULL || !h->user[0]) {
        ESP_LOGE(TAG, "incomplete configuration: a Host with a User in the SD card's .ssh/config");
        return;
    }
    if (state.wifi == NULL || wifi_connect(state.wifi, 15000) != ESP_OK) {
        ESP_LOGW(TAG, "no known Wi-Fi network answered: waiting for lte on");
        xSemaphoreTake(online, portMAX_DELAY);
    }

    char* private_key = storage_get_private_key(h->identity_file);
    char password[128] = "";
    if (private_key == NULL) {
        char prompt[300];
        snprintf(prompt, sizeof prompt, "%s@%s's password: ", h->user, h->hostname);
        read_line(prompt, password, sizeof password, 0);
    }
    char* known_hosts = storage_get_known_hosts();
    session = (ssh_session){.known_hosts = known_hosts, .private_key = private_key, .password = password[0] ? password : NULL};
    state.session = &session;
    int rc = ssh_open(&state);
    if (private_key != NULL) wc_ForceZero(private_key, strlen(private_key));
    free(private_key);
    free(known_hosts);
    memset(password, 0, sizeof password);
    if (rc == WS_SUCCESS) {
        ESP_LOGI(TAG, "connected to %s", h->hostname);
        esp_log_level_set("*", ESP_LOG_WARN);
        // They answer control commands
        esp_log_level_set("catm", ESP_LOG_INFO);
        esp_log_level_set("net", ESP_LOG_INFO);
        bridge();
        esp_log_level_set("*", ESP_LOG_INFO);
        int exit_status;
        ssh_close(&state, &exit_status);
        ESP_LOGI(TAG, "exit status %d", exit_status);
    } else {
        ESP_LOGE(TAG, "ssh %s@%s:%u: %s", h->user, h->hostname, h->port, wolfSSH_ErrorToName(rc));
    }
    state.session = NULL;
}

static bool command(const char* line) {
    char host[64];
    uint16_t port;
    if (strcmp(line, "lte on") == 0) {
        esp_err_t err = catm_connect();
        ESP_LOGI(TAG, "lte on: %s", esp_err_to_name(err));
        if (err == ESP_OK) xSemaphoreGive(online);
    } else if (strcmp(line, "lte off") == 0) {
        catm_disconnect();
    } else if (strcmp(line, "lte status") == 0) {
        ESP_LOGI(TAG, "lte status: %s", esp_err_to_name(catm_status()));
    } else if (sscanf(line, "net check %63s %hu", host, &port) == 2) {
        net_check(host, port);
    } else {
        return false;
    }
    return true;
}

static void run_session(void* arg) {
    (void)arg;
    connect();
    vTaskDelete(NULL);
}

void app_main(void) {
    ESP_ERROR_CHECK(keys_start());
    esp_err_t keyboard = keyboard_usb_start();
    online = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(usb_start(command));
    if (keyboard != ESP_OK) ESP_LOGE(TAG, "no USB keyboard: %s", esp_err_to_name(keyboard));
    if (keyboard_tab5_start() != ESP_OK) ESP_LOGE(TAG, "no Tab5 keyboard");
    ESP_ERROR_CHECK(nvs_flash_init());
    if (storage_mount(usb_storage_boot()) != 0) ESP_LOGI(TAG, "no FAT SD card");
    if (usb_storage_boot()) return;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
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
