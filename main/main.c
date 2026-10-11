// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <driver/usb_serial_jtag.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <nvs_flash.h>
#include <wolfssl/wolfcrypt/memory.h>

#include "config.h"
#include "ssh.h"
#include "storage.h"
#include "wifi.h"

static const char* TAG = "main";
static ssh_session session;
static global_state state = {
    .terminal_type = "xterm-256color",
    .terminal_size = {.columns = 106, .rows = 30, .width_pixels = 1272, .height_pixels = 720},
};

static void read_line(const char* prompt, char* buf, size_t size, int echo) {
    usb_serial_jtag_write_bytes(prompt, strlen(prompt), portMAX_DELAY);
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') break;
        if (c == 0x7f || c == '\b') {
            if (n == 0) continue;
            n--;
            if (echo) usb_serial_jtag_write_bytes("\b \b", 3, portMAX_DELAY);
        } else if (n + 1 < size) {
            buf[n++] = (char)c;
            if (echo) usb_serial_jtag_write_bytes(&c, 1, portMAX_DELAY);
        }
    }
    buf[n] = 0;
    usb_serial_jtag_write_bytes("\r\n", 2, portMAX_DELAY);
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
            pending = usb_serial_jtag_read_bytes(in, sizeof in, 0);
            off = 0;
        }
        int sent = pending > 0 ? ssh_write(&state, in + off, (size_t)pending) : WS_WANT_WRITE;
        int got = ssh_read(&state, out, sizeof out);
        if ((sent < 0 && sent != WS_WANT_WRITE) || (got < 0 && got != WS_WANT_READ)) return;
        if (sent > 0) {
            off += sent;
            pending -= sent;
        }
        if (got > 0)
            usb_serial_jtag_write_bytes(out, (size_t)got, portMAX_DELAY);
        else if (sent < 0)
            ssh_wait(&state, 10);
    }
}

void app_main(void) {
    static const char* const sources[] = {"nothing", "the SD card", "NVS"};
    ESP_ERROR_CHECK(nvs_flash_init());
    if (storage_mount() != 0) ESP_LOGI(TAG, "no FAT SD card");
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
    if (state.wifi == NULL || h == NULL || !h->user[0]) {
        ESP_LOGE(TAG, "incomplete configuration: Wi-Fi networks, and a Host with a User in the SD card's .ssh/config");
        return;
    }
    if (wifi_connect(state.wifi, 15000) != ESP_OK) {
        ESP_LOGE(TAG, "no known Wi-Fi network answered");
        return;
    }

    // The driver drops input that does not fit; nothing tells the host to slow down
    usb_serial_jtag_driver_config_t usb = {.rx_buffer_size = 16384, .tx_buffer_size = 4096};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
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
