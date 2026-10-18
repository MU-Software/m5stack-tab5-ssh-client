// SPDX-License-Identifier: GPL-3.0-or-later
#include "c6.h"

#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include <eh_host_event.h>
#include <eh_host_feat_nw_split.h>
#include <eh_host_feat_peer_data.h>
#include <eh_host_sys.h>
#include <esp_attr.h>
#include <esp_check.h>
#include <esp_event.h>
#include <esp_hosted.h>
#include <esp_hosted_host_fw_ver.h>
#include <esp_hosted_ota.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "c6_protocol.h"

#define RESTARTED   0x43365253
#define NVS_WAIT_MS 3000

static const char* TAG = "c6";
static void* bus;
static volatile unsigned protocol;
// The C6 was reset this boot, so its SDIO counters start from zero
static bool reset_done;
static c6_wifi_status wifi_status;
static unsigned wifi_reports;
static void (*wifi_listener)(void);
static portMUX_TYPE wifi_lock = portMUX_INITIALIZER_UNLOCKED;
static void (*lease_listener)(const c6_lease* lease);
// Set by the restart that follows a C6 that did not come up, so one that stays silent is reset only once
static RTC_NOINIT_ATTR uint32_t restarted;
// One P4_NVS at a time; the answer goes to nvs_answer while it is set
static StaticSemaphore_t nvs_lock_memory, nvs_answered_memory;
static SemaphoreHandle_t nvs_lock, nvs_answered;
static portMUX_TYPE nvs_guard = portMUX_INITIALIZER_UNLOCKED;
static uint8_t nvs_sequence, *nvs_answer;
static size_t nvs_answer_length;
static esp_err_t nvs_error;
EXT_RAM_BSS_ATTR static uint8_t nvs_message[2 + C6_NVS_MESSAGE];
// One c6_internet or c6_join at a time waits for the answer of its kind and sequence
static StaticSemaphore_t ask_lock_memory, asked_memory;
static SemaphoreHandle_t ask_lock, asked;
static portMUX_TYPE ask_guard = portMUX_INITIALIZER_UNLOCKED;
static uint8_t ask_kind, ask_sequence, ask_answer[8];
static bool ask_waiting;

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
    if (size >= 2 && (data[0] == C6_INTERNET || data[0] == C6_WIFI_JOINED)) {
        bool answered = false;
        taskENTER_CRITICAL(&ask_guard);
        if (ask_waiting && data[0] == ask_kind && data[1] == ask_sequence) {
            memcpy(ask_answer, data + 2, MIN(size - 2, sizeof ask_answer));
            ask_waiting = false;
            answered = true;
        }
        taskEXIT_CRITICAL(&ask_guard);
        if (answered) xSemaphoreGive(asked);
    }
    if (size >= 6 && data[0] == C6_NVS) {
        bool answered = false;
        taskENTER_CRITICAL(&nvs_guard);
        if (nvs_answer != NULL && data[1] == nvs_sequence) {
            nvs_error = (esp_err_t)(data[2] | data[3] << 8 | data[4] << 16 | (uint32_t)data[5] << 24);
            memcpy(nvs_answer, data + 6, nvs_answer_length = MIN(size - 6, nvs_answer_length));
            nvs_answer = NULL;
            answered = true;
        }
        taskEXIT_CRITICAL(&nvs_guard);
        if (answered) xSemaphoreGive(nvs_answered);
    }
    for (; size >= 1 && data[0] == C6_LOG && line < end; line = eol + 1) {
        eol = memchr(line, '\n', (size_t)(end - line));
        if (eol == NULL) eol = end;
        if (eol > line) ESP_LOGI("C6", "%.*s", (int)(eol - line), line);
    }
}

// Each SDIO transfer, or c6_hold_transfers: light sleep stops the SDMMC clock under a transfer, which then times out
static StaticSemaphore_t transfer_lock_memory;
static SemaphoreHandle_t transfer_lock;

// Before any task, so no transfer goes past it
__attribute__((constructor)) static void make_transfer_lock(void) {
    transfer_lock = xSemaphoreCreateMutexStatic(&transfer_lock_memory);
}

bool c6_hold_transfers(uint32_t milliseconds) {
    return xSemaphoreTake(transfer_lock, pdMS_TO_TICKS(milliseconds)) == pdTRUE;
}

void c6_release_transfers(void) {
    xSemaphoreGive(transfer_lock);
}

// esp_hosted's port layer; its headers are private
// NOLINTBEGIN(bugprone-reserved-identifier): --wrap names them
#define LOCKED(name)                                                                               \
    int __real_##name(void* ctx, uint32_t reg, uint8_t* data, uint16_t size, bool lock_required);  \
    int __wrap_##name(void* ctx, uint32_t reg, uint8_t* data, uint16_t size, bool lock_required) { \
        xSemaphoreTake(transfer_lock, portMAX_DELAY);                                              \
        int err = __real_##name(ctx, reg, data, size, lock_required);                              \
        xSemaphoreGive(transfer_lock);                                                             \
        return err;                                                                                \
    }
LOCKED(eh_host_port_sdio_read_reg)
LOCKED(eh_host_port_sdio_write_reg)
LOCKED(eh_host_port_sdio_read_block)
LOCKED(eh_host_port_sdio_write_block)
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

static void on_status(void* arg, esp_event_base_t base, int32_t id, void* data) {
    const eh_host_nw_split_status_t* s = data;
    (void)arg, (void)base, (void)id;
    c6_lease lease = {0};
    if (s->dhcp_up && s->has_ipv4)
        lease = (c6_lease){.address = s->ipv4.ip.addr,
                           .netmask = s->ipv4.netmask.addr,
                           .gateway = s->ipv4.gw.addr,
                           .dns = s->dns_main.type == ESP_IPADDR_TYPE_V4 ? s->dns_main.u_addr.ip4.addr : 0};
    if (lease_listener != NULL) lease_listener(&lease);
}

void c6_on_lease(void (*listener)(const c6_lease* lease)) {
    lease_listener = listener;
}

esp_err_t c6_ask_lease(void) {
    eh_host_nw_split_status_t status;
    if (protocol < C6_NW_SPLIT_SINCE) return ESP_ERR_NOT_SUPPORTED;
    ESP_RETURN_ON_ERROR(eh_host_nw_split_get_status(&status), TAG, "lease");
    return esp_event_post(EH_HOST_EVENT, EH_HOST_EVENT_NW_SPLIT_STATUS, &status, sizeof status, pdMS_TO_TICKS(100));
}

// The C6 keeps its power and Wi-Fi through restarts of this chip (power.c powers it)
esp_err_t c6_start(void) {
    char version[24] = "unknown";
    nvs_lock = xSemaphoreCreateMutexStatic(&nvs_lock_memory);
    nvs_answered = xSemaphoreCreateBinaryStatic(&nvs_answered_memory);
    ask_lock = xSemaphoreCreateMutexStatic(&ask_lock_memory);
    asked = xSemaphoreCreateBinaryStatic(&asked_memory);
    ESP_RETURN_ON_ERROR(esp_event_handler_register(EH_HOST_EVENT, EH_HOST_EVENT_NW_SPLIT_STATUS, on_status, NULL), TAG, "lease");
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

// The answer comes on its own, often before the send returns
esp_err_t c6_nvs(const uint8_t* request, size_t request_length, uint8_t* answer, size_t* answer_length) {
    if (protocol < C6_NVS_SINCE) return ESP_ERR_NOT_SUPPORTED;
    if (request_length > C6_NVS_MESSAGE) return ESP_ERR_INVALID_SIZE;
    xSemaphoreTake(nvs_lock, portMAX_DELAY);
    xSemaphoreTake(nvs_answered, 0);
    nvs_message[0] = P4_NVS;
    nvs_message[1] = ++nvs_sequence;
    memcpy(nvs_message + 2, request, request_length);
    taskENTER_CRITICAL(&nvs_guard);
    nvs_answer = answer;
    nvs_answer_length = *answer_length;
    taskEXIT_CRITICAL(&nvs_guard);
    esp_err_t err = eh_host_peer_data_send(P4_TO_C6, nvs_message, 2 + request_length);
    memset(nvs_message, 0, sizeof nvs_message);
    if (err == ESP_OK && xSemaphoreTake(nvs_answered, pdMS_TO_TICKS(NVS_WAIT_MS)) != pdTRUE) err = ESP_ERR_TIMEOUT;
    taskENTER_CRITICAL(&nvs_guard);
    if (err == ESP_OK) err = nvs_error;
    *answer_length = err == ESP_OK ? nvs_answer_length : 0;
    nvs_answer = NULL;
    taskEXIT_CRITICAL(&nvs_guard);
    xSemaphoreGive(nvs_lock);
    return err;
}

// message's kind first, its sequence second (set here); the body of the answer of kind with that sequence into answer
static esp_err_t ask(uint8_t* message, size_t size, uint8_t kind, int timeout_ms, void (*meanwhile)(void* context), void* context,
                     uint8_t answer[sizeof ask_answer]) {
    static uint8_t sequence;
    xSemaphoreTake(ask_lock, portMAX_DELAY);
    xSemaphoreTake(asked, 0);
    taskENTER_CRITICAL(&ask_guard);
    // 1-255: 0 is the C6's own
    message[1] = ask_sequence = sequence = (uint8_t)(sequence % 255 + 1);
    ask_kind = kind;
    ask_waiting = true;
    taskEXIT_CRITICAL(&ask_guard);
    TickType_t start = xTaskGetTickCount();
    esp_err_t err = eh_host_peer_data_send(P4_TO_C6, message, size);
    if (meanwhile != NULL) meanwhile(context);
    TickType_t spent = xTaskGetTickCount() - start, limit = pdMS_TO_TICKS(timeout_ms);
    if (err == ESP_OK && xSemaphoreTake(asked, spent < limit ? limit - spent : 0) != pdTRUE) err = ESP_ERR_TIMEOUT;
    taskENTER_CRITICAL(&ask_guard);
    ask_waiting = false;
    if (err == ESP_OK) memcpy(answer, ask_answer, sizeof ask_answer);
    taskEXIT_CRITICAL(&ask_guard);
    xSemaphoreGive(ask_lock);
    return err;
}

esp_err_t c6_internet(int timeout_ms, void (*meanwhile)(void* context), void* context, c6_reach_answer* answer) {
    uint8_t message[2] = {P4_INTERNET}, body[sizeof ask_answer];
    if (protocol < C6_INTERNET_SINCE) return ESP_ERR_NOT_SUPPORTED;
    ESP_RETURN_ON_ERROR(ask(message, sizeof message, C6_INTERNET, timeout_ms, meanwhile, context, body), TAG, "internet");
    *answer = (c6_reach_answer){.error = body[0], .milliseconds = body[1] | body[2] << 8};
    snprintf(answer->address, sizeof answer->address, "%u.%u.%u.%u", body[3], body[4], body[5], body[6]);
    return ESP_OK;
}

esp_err_t c6_join(const char* ssid, const char* password, int timeout_ms, uint8_t* reason) {
    uint8_t message[4 + 32 + 64] = {P4_WIFI_JOIN}, body[sizeof ask_answer];
    size_t s = strlen(ssid), p = strlen(password);
    if (protocol < C6_JOIN_SINCE) return ESP_ERR_NOT_SUPPORTED;
    if (s == 0 || s > 32 || p > 64) return ESP_ERR_INVALID_ARG;
    message[2] = (uint8_t)s;
    memcpy(message + 3, ssid, s);
    message[3 + s] = (uint8_t)p;
    memcpy(message + 4 + s, password, p);
    esp_err_t err = ask(message, 4 + s + p, C6_WIFI_JOINED, timeout_ms, NULL, NULL, body);
    memset(message, 0, sizeof message);
    if (err != ESP_OK) return err;
    *reason = body[1];
    return body[0] == C6_JOIN_DONE        ? ESP_OK
           : body[0] == C6_JOIN_NOT_FOUND ? ESP_ERR_NOT_FOUND
           : body[0] == C6_JOIN_STOPPED   ? ESP_ERR_INVALID_STATE
                                          : ESP_FAIL;
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
