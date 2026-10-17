// SPDX-License-Identifier: Apache-2.0
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <driver/sdio_slave.h>
#include <eh_cp_feat_peer_data.h>
#include <eh_transport_cp.h>
#include <esp_attr.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "c6_protocol.h"

#define RING 4096

static const char* TAG = "tab5";
// What this chip logged, sent on to the P4 once it reached us; the oldest goes when full
static char ring[RING];
static size_t head, used;
static portMUX_TYPE ring_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t uart;
static TaskHandle_t sender;
static volatile bool p4_reached;

static int capture(const char* format, va_list args) {
    char line[160];
    va_list copy;
    va_copy(copy, args);
    int n = uart(format, args);
    // Not what sending logs, which would send again
    int length = xTaskGetCurrentTaskHandle() != sender ? vsnprintf(line, sizeof line, format, copy) : 0;
    va_end(copy);
    if (length > (int)sizeof line - 1) length = sizeof line - 1;
    portENTER_CRITICAL(&ring_lock);
    for (int i = 0; i < length; i++)
        if (used < RING)
            ring[(head + used++) % RING] = line[i];
        else
            ring[head] = line[i], head = (head + 1) % RING;
    portEXIT_CRITICAL(&ring_lock);
    return n;
}

// Whole lines, unless one fills a message
static void send_logs(void* arg) {
    static uint8_t message[1 + 1000];
    (void)arg;
    for (;;) {
        size_t n = 0;
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!p4_reached) continue;
        portENTER_CRITICAL(&ring_lock);
        for (; n < used && n < sizeof message - 1; n++)
            message[1 + n] = (uint8_t)ring[(head + n) % RING];
        for (size_t end = n; end > 0; end--)
            if (message[end] == '\n') {
                n = end;
                break;
            }
        head = (head + n) % RING, used -= n;
        portEXIT_CRITICAL(&ring_lock);
        message[0] = C6_LOG;
        if (n > 0 && eh_cp_feat_peer_data_send(C6_TO_P4, message, n + 1) != ESP_OK) p4_reached = false;
    }
}

_Static_assert(C6_SDIO_RESET == ESP_RESET && C6_SDIO_CLOSE == ESP_CLOSE_DATA_PATH, "esp_hosted's host interrupts");

// esp_hosted's core; no header declares it
extern interface_context_t* if_context;
static sdio_event_cb_t esp_hosted_event;
static TaskHandle_t resetter;

static void IRAM_ATTR from_host(uint8_t event) {
    BaseType_t woken = pdFALSE;
    if (event != ESP_RESET) {
        esp_hosted_event(event);
        return;
    }
    vTaskNotifyGiveFromISR(resetter, &woken);
    portYIELD_FROM_ISR(woken);
}

// After each card init (components/c6/c6.c). esp_hosted restarts the counters within the interrupt, where it takes back the
// send that its sdio_send task waits for, which then holds the transmit lock for good
static void reset_sdio(void* arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if_context->event_handler(ESP_CLOSE_DATA_PATH);
        // Each round returns a send in flight to its sender, which may queue another
        for (int i = 0; i < 3; i++) {
            sdio_slave_stop();
            sdio_slave_reset();
            sdio_slave_set_host_intena(SDIO_SLAVE_HOSTINT_SEND_NEW_PACKET | SDIO_SLAVE_HOSTINT_BIT0 | SDIO_SLAVE_HOSTINT_BIT1 |
                                       SDIO_SLAVE_HOSTINT_BIT2 | SDIO_SLAVE_HOSTINT_BIT3 | SDIO_SLAVE_HOSTINT_BIT4 | SDIO_SLAVE_HOSTINT_BIT5 |
                                       SDIO_SLAVE_HOSTINT_BIT6 | SDIO_SLAVE_HOSTINT_BIT7);
            sdio_slave_start();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        sdio_slave_write_reg(0, C6_RESET_DONE);
        ESP_LOGI(TAG, "SDIO counters restarted for the host");
    }
}

// NOLINTBEGIN(bugprone-reserved-identifier): --wrap names them
esp_err_t __real_sdio_slave_initialize(sdio_slave_config_t* config);
esp_err_t __wrap_sdio_slave_initialize(sdio_slave_config_t* config) {
    // Above esp_hosted's tasks (21), so a round runs whole
    if (resetter == NULL && xTaskCreate(reset_sdio, "reset sdio", 3072, NULL, 22, &resetter) != pdPASS) return ESP_ERR_NO_MEM;
    esp_hosted_event = config->event_cb;
    config->event_cb = from_host;
    return __real_sdio_slave_initialize(config);
}
// NOLINTEND(bugprone-reserved-identifier)

static bool unchecked(void) {
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK && state != ESP_OTA_IMG_VALID &&
           state != ESP_OTA_IMG_UNDEFINED;
}

// The C6 can only be reflashed through the back-cover pads: an image is kept once the P4 reached it through esp_hosted, which
// any later update needs
// Count of networks in a P4_WIFI_LIST body, or -1 when it does not parse
static int networks(const uint8_t* list, size_t size) {
    size_t at = 2;
    if (size < 2 || list[1] > C6_NETWORKS) return -1;
    for (int i = 0; i < list[1]; i++)
        for (int field = 0, longest = 32; field < 2; field++, longest = 64) {
            if (at >= size || list[at] > longest || at + 1 + list[at] > size) return -1;
            at += 1 + list[at];
        }
    return at == size ? list[1] : -1;
}

static void keep_list(const uint8_t* list, size_t size) {
    static uint8_t kept[2 + C6_NETWORKS * 98];
    size_t length = sizeof kept;
    nvs_handle_t nvs;
    int count = networks(list, size);
    if (count < 0 || nvs_open("tab5", NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "known networks refused");
        return;
    }
    bool same = nvs_get_blob(nvs, "wifi", kept, &length) == ESP_OK && length == size && memcmp(kept, list, size) == 0;
    esp_err_t err = same ? ESP_OK : nvs_set_blob(nvs, "wifi", list, size);
    if (err == ESP_OK && !same) err = nvs_commit(nvs);
    nvs_close(nvs);
    memset(kept, 0, sizeof kept);
    ESP_LOGI(TAG, "%d known networks, Wi-Fi %s: %s", count, list[0] ? "on" : "off", same ? "unchanged" : esp_err_to_name(err));
}

static void from_p4(uint32_t id, const uint8_t* data, size_t size, void* context) {
    (void)id, (void)context;
    if (size >= 1 && data[0] == P4_WIFI_LIST) keep_list(data + 1, size - 1);
    if (size < 1 || data[0] != P4_KEEP) return;
    if (unchecked() && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) ESP_LOGI(TAG, "image kept");
    eh_cp_feat_peer_data_send(C6_TO_P4, (const uint8_t[]){C6_HELLO, C6_PROTOCOL}, 2);
    p4_reached = true;
}

// The C6's bootloader has no rollback (a new image stays NEW): an image the P4 has not reached within a minute boots the other
// one, unless that one is not a valid image
static void await_p4(void* arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(60000));
    const esp_partition_t* other = esp_ota_get_next_update_partition(NULL);
    if (unchecked() && other != NULL) {
        ESP_LOGE(TAG, "the P4 never reached this image: back to %s", other->label);
        if (esp_ota_set_boot_partition(other) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(500)); // for the log to reach the P4
            esp_restart();
        }
    }
    vTaskDelete(NULL);
}

void app_main(void) {
    xTaskCreate(send_logs, "send logs", 3072, NULL, 2, &sender);
    uart = esp_log_set_vprintf(capture);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_get_state_partition(running, &state);
    ESP_LOGI(TAG, "running %s, state %d, boot %s, next %s, reset reason %d", running->label, state, esp_ota_get_boot_partition()->label,
             esp_ota_get_next_update_partition(NULL)->label, esp_reset_reason());
    nvs_stats_t stats;
    if (nvs_get_stats(NULL, &stats) == ESP_OK)
        ESP_LOGI(TAG, "NVS %u of %u entries free, heap %u free", (unsigned)stats.free_entries, (unsigned)stats.total_entries,
                 (unsigned)esp_get_free_heap_size());
    if (state != ESP_OTA_IMG_VALID) xTaskCreate(await_p4, "await P4", 3072, NULL, 2, NULL);
    ESP_ERROR_CHECK(eh_cp_feat_peer_data_init());
    ESP_ERROR_CHECK(eh_cp_feat_peer_data_register_callback(P4_TO_C6, from_p4, NULL));
}
