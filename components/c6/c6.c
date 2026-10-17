// SPDX-License-Identifier: GPL-3.0-or-later
#include "c6.h"

#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include <eh_host_feat_peer_data.h>
#include <eh_host_sys.h>
#include <esp_attr.h>
#include <esp_check.h>
#include <esp_hosted.h>
#include <esp_hosted_host_fw_ver.h>
#include <esp_hosted_ota.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "c6_protocol.h"

#define RESTARTED 0x43365253

static const char* TAG = "c6";
static void* bus;
static volatile unsigned protocol;
// The C6 was reset this boot, so its SDIO counters start from zero
static bool reset_done;
static c6_wifi_status wifi_status;
static unsigned wifi_reports;
static void (*wifi_listener)(void);
static portMUX_TYPE wifi_lock = portMUX_INITIALIZER_UNLOCKED;
// Set by the restart that follows a C6 that did not come up, so one that stays silent is reset only once
static RTC_NOINIT_ATTR uint32_t restarted;

bool c6_version(char* version, size_t size) {
    eh_host_coprocessor_fwver_t v = {0};
    if (eh_host_sys_get_cp_fw_version(&v) != ESP_OK) {
        snprintf(version, size, "unknown");
        return false;
    }
    snprintf(version, size, "%lu.%lu.%lu", v.major1, v.minor1, v.patch1);
    return v.major1 == ESP_HOSTED_VERSION_MAJOR_1;
}

static void from_c6(uint32_t id, const uint8_t* data, size_t size, void* context) {
    (void)id, (void)context;
    const char *line = (const char*)data + 1, *end = (const char*)data + size, *eol;
    if (size >= 2 && data[0] == C6_HELLO) ESP_LOGI(TAG, "protocol %u", protocol = data[1]);
    if (size >= 5 && data[0] == C6_WIFI_STATE && data[4] <= 32 && size == 5u + data[4]) {
        c6_wifi_status s = {.state = data[1], .reason = data[2], .rssi = (int8_t)data[3]};
        memcpy(s.ssid, data + 5, data[4]);
        taskENTER_CRITICAL(&wifi_lock);
        wifi_status = s;
        wifi_reports++;
        taskEXIT_CRITICAL(&wifi_lock);
        if (wifi_listener != NULL) wifi_listener();
    }
    for (; size >= 1 && data[0] == C6_LOG && line < end; line = eol + 1) {
        eol = memchr(line, '\n', (size_t)(end - line));
        if (eol == NULL) eol = end;
        if (eol > line) ESP_LOGI("C6", "%.*s", (int)(eol - line), line);
    }
}

// esp_hosted's port layer; its headers are private
// NOLINTBEGIN(bugprone-reserved-identifier): --wrap names them
int __real_eh_host_port_sdio_card_init(void* ctx, bool show_config);
int eh_host_port_sdio_read_reg(void* ctx, uint32_t reg, uint8_t* data, uint16_t size, bool lock_required);
int eh_host_port_sdio_write_reg(void* ctx, uint32_t reg, uint8_t* data, uint16_t size, bool lock_required);
int eh_host_port_reset_slave(void);

// A C6 that ran on through this chip's restart still counts the SDIO transfers of the boot before; one that does not answer
// the reset of its counters is reset and booted again
int __wrap_eh_host_port_sdio_card_init(void* ctx, bool show_config) {
    int err = __real_eh_host_port_sdio_card_init(ctx, show_config);
    uint8_t answer = 0, reset = 1 << C6_SDIO_RESET;
    if (err != 0 || reset_done) return err;
    bus = ctx;
    eh_host_port_sdio_write_reg(ctx, C6_SCRATCH, &answer, 1, false);
    eh_host_port_sdio_write_reg(ctx, C6_INTERRUPT, &reset, 1, false);
    // A C6 that is restarting does not answer reads at all
    for (int i = 0; i < 20 && answer != C6_RESET_DONE; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (eh_host_port_sdio_read_reg(ctx, C6_SCRATCH, &answer, 1, false) != 0) break;
    }
    if (answer == C6_RESET_DONE) return 0;
    ESP_LOGW(TAG, "no answer to the SDIO reset: resetting the C6");
    reset_done = true;
    eh_host_port_reset_slave();
    return -1;
}
// NOLINTEND(bugprone-reserved-identifier)

// The C6 stops queueing frames for a host that will not read them
static void leaving(void) {
    uint8_t close = 1 << C6_SDIO_CLOSE;
    if (bus != NULL && eh_host_port_sdio_write_reg(bus, C6_INTERRUPT, &close, 1, true) == 0) vTaskDelay(pdMS_TO_TICKS(20));
}

// The C6 keeps its power and Wi-Fi through restarts of this chip (power.c powers it)
esp_err_t c6_start(void) {
    char version[24] = "unknown";
    esp_err_t err = esp_hosted_init();
    if (err == ESP_OK) err = esp_hosted_connect_to_slave();
    bool known = err == ESP_OK && c6_version(version, sizeof version);
    if (strcmp(version, "unknown") == 0 && restarted != RESTARTED) {
        ESP_LOGE(TAG, "no answer (%s): resetting the C6 and restarting", esp_err_to_name(err));
        restarted = RESTARTED;
        eh_host_port_reset_slave();
        vTaskDelay(pdMS_TO_TICKS(500)); // for the log writer
        esp_restart();
    }
    restarted = 0;
    ESP_RETURN_ON_ERROR(err, TAG, "connect");
    esp_register_shutdown_handler(leaving);
    if (known)
        ESP_LOGI(TAG, "firmware %s", version);
    else
        ESP_LOGW(TAG, "firmware %s, host %d.x: make c6-update", version, ESP_HOSTED_VERSION_MAJOR_1);
    // The C6 keeps a new image only once this reaches it
    if (eh_host_peer_data_register(C6_TO_P4, from_c6, NULL) != ESP_OK || eh_host_peer_data_send(P4_TO_C6, (const uint8_t[]){P4_KEEP}, 1) != ESP_OK) {
        ESP_LOGW(TAG, "a C6 firmware without tools/c6's messages: make c6-update");
        return ESP_OK;
    }
    // Its answer comes on its own
    for (int i = 0; i < 50 && protocol == 0; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

unsigned c6_protocol(void) {
    return protocol;
}

esp_err_t c6_send(const uint8_t* message, size_t size) {
    return eh_host_peer_data_send(P4_TO_C6, message, size);
}

unsigned c6_wifi_state(c6_wifi_status* status) {
    taskENTER_CRITICAL(&wifi_lock);
    unsigned reports = wifi_reports;
    if (status != NULL) *status = wifi_status;
    taskEXIT_CRITICAL(&wifi_lock);
    return reports;
}

void c6_on_wifi_state(void (*listener)(void)) {
    wifi_listener = listener;
}

esp_err_t c6_update(const uint8_t* image, size_t size) {
    eh_host_coprocessor_fwver_t v = {0};
    eh_host_sys_get_cp_fw_version(&v);
    ESP_LOGI(TAG, "firmware %lu.%lu.%lu, writing %u bytes", v.major1, v.minor1, v.patch1, (unsigned)size);
    ESP_RETURN_ON_ERROR(esp_hosted_slave_ota_begin(), TAG, "begin");
    for (size_t off = 0; off < size; off += 1500) {
        esp_err_t err = esp_hosted_slave_ota_write(image + off, MIN(size - off, 1500));
        if (err != ESP_OK) {
            esp_hosted_slave_ota_end();
            return err;
        }
    }
    ESP_RETURN_ON_ERROR(esp_hosted_slave_ota_end(), TAG, "end");
    // Activation only works in the session that wrote the image; esp_hosted 1.x refuses it and boots the image on reset
    esp_err_t err = esp_hosted_slave_ota_activate();
    if (err != ESP_OK) ESP_LOGW(TAG, "activate: %s; the new firmware runs after the next reset", esp_err_to_name(err));
    return ESP_OK;
}
