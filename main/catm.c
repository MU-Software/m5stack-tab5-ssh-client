// SPDX-License-Identifier: GPL-3.0-or-later
#include "catm.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_check.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_ppp.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "modem.h"

#define UART   UART_NUM_2
#define TX     6
#define RX     46
#define LINE   128
#define SECOND 1000000LL
#define GOT_IP BIT0
#define DEAD   BIT1

static const char* TAG = "catm";
// An LCP Terminate-Request with every control byte escaped: a modem left in PPP by an earlier boot answers NO CARRIER,
// one in command mode ignores it
static const uint8_t terminate[] = {0x7e, 0xff, 0x7d, 0x23, 0xc0, 0x21, 0x7d, 0x25, 0x7d, 0x21, 0x7d, 0x20, 0x7d, 0x24, 0x3d, 0xc7, 0x7e};
static QueueHandle_t lines;
static EventGroupHandle_t events;
static esp_netif_t* netif;
// One user of the modem at a time: the LTE task dials while a control command may ask for the status
static SemaphoreHandle_t lock;
static atomic_bool dialing, data_mode;
// When the modem last left a command unanswered: asking a quiet modem again keeps it quiet longer
static int64_t unanswered = -90 * SECOND;
static atomic_bool answered; // this boot
// The modem spoke unasked, e.g. NO CARRIER once a PPP link of an earlier boot timed out: it can be asked again at once
static atomic_bool heard;
// Read under lock, copied out under guard: a dial holds lock for a minute
EXT_RAM_BSS_ATTR static modem_snapshot fresh, shown;
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static bool kept; // this boot

static void reader(void* arg) {
    (void)arg;
    uint8_t buffer[512];
    char line[LINE];
    size_t n = 0;
    for (;;) {
        int got = uart_read_bytes(UART, buffer, sizeof buffer, pdMS_TO_TICKS(20));
        for (int i = 0; i < got; i++) {
            if (data_mode) {
                esp_netif_receive(netif, buffer + i, (size_t)(got - i), NULL);
                break;
            }
            if (buffer[i] != '\r' && buffer[i] != '\n') {
                if (n + 1 < LINE) line[n++] = isprint(buffer[i]) ? (char)buffer[i] : '.';
                continue;
            }
            if (n == 0) continue;
            line[n] = 0;
            n = 0;
            char logged[LINE];
            modem_mask(line, logged, sizeof logged);
            ESP_LOGI(TAG, "< %s", logged);
            if (dialing && strncmp(line, "CONNECT", 7) == 0) data_mode = true;
            // Only a modem sends these: noise on a pin without one would not
            if (strcmp(line, "OK") == 0 || strcmp(line, "NO CARRIER") == 0 || strcmp(line, "RDY") == 0 || line[0] == '+' || line[0] == '$')
                atomic_store(&heard, true), atomic_store(&answered, true);
            xQueueSend(lines, line, 0);
        }
    }
}

static bool receive(char* line, int64_t deadline) {
    int64_t left = deadline - esp_timer_get_time();
    return left > 0 && xQueueReceive(lines, line, pdMS_TO_TICKS(left / 1000)) == pdTRUE;
}

static void send(const char* command) {
    while (xQueueReceive(lines, (char[LINE]){0}, 0) == pdTRUE) {}
    ESP_LOGI(TAG, "> %s", command);
    uart_write_bytes(UART, command, strlen(command));
    uart_write_bytes(UART, "\r", 1);
}

// The final result OK, with the line starting with prefix in reply
static bool at(const char* command, const char* prefix, char* reply) {
    char line[LINE];
    if (reply != NULL) reply[0] = 0;
    send(command);
    for (int64_t deadline = esp_timer_get_time() + 3 * SECOND; receive(line, deadline);) {
        if (reply != NULL && strncmp(line, prefix, strlen(prefix)) == 0) strlcpy(reply, line, LINE);
        if (strcmp(line, "OK") == 0) return true;
        if (strcmp(line, "ERROR") == 0 || strncmp(line, "+CME ERROR", 10) == 0) return false;
    }
    unanswered = esp_timer_get_time(), atomic_store(&heard, false);
    return false;
}

// The item into fresh: 1 for OK, 0 for ERROR (the value then empty), -1 when left unanswered (the value kept)
static int ask(modem_item item) {
    char reply[LINE];
    int64_t before = unanswered;
    bool ok = at(modem_commands[item].read, modem_commands[item].prefix, reply);
    if (unanswered != before) return -1;
    modem_value(reply, modem_commands[item].prefix, fresh.values[item], sizeof fresh.values[item]);
    fresh.read_at[item] = esp_timer_get_time();
    return ok;
}

// CTRJ 0 alone proves nothing: its read gives 0 while its gate is unset
static esp_err_t backoff_clear(void) {
    long trj = -1;
    if (ask(MODEM_REJECTS) <= 0 || ask(MODEM_BACKOFF) <= 0) return ESP_ERR_TIMEOUT;
    sscanf(fresh.values[MODEM_REJECTS], "%ld", &trj);
    if (trj != 0 || !modem_backoff_clear(fresh.values[MODEM_BACKOFF])) {
        ESP_LOGE(TAG, "a network back-off may be running: nothing sent");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

// Present, registered, and with the default bearer up, as the research bench required before every dial
static esp_err_t check(void) {
    int stat = -1;
    esp_err_t err = backoff_clear();
    if (err != ESP_OK) return err;
    if (ask(MODEM_REGISTRATION) <= 0 || ask(MODEM_SYSTEM) <= 0 || ask(MODEM_BEARER) <= 0) return ESP_ERR_TIMEOUT;
    sscanf(fresh.values[MODEM_REGISTRATION], "%*d,%d", &stat);
    // The short form stays for ~20 s after the radio is lost
    if ((stat != 1 && stat != 5) || strncmp(fresh.values[MODEM_SYSTEM], "LTE CAT-M1,Online,450-", 22) != 0 ||
        strcmp(fresh.values[MODEM_BEARER], "1") != 0) {
        ESP_LOGW(TAG, "not registered with the default bearer up yet");
        return ESP_ERR_NOT_FINISHED;
    }
    return ESP_OK;
}

static esp_err_t open_uart(void) {
    static bool open;
    if (open) return ESP_OK;
    ESP_RETURN_ON_ERROR(uart_driver_install(UART, 16384, 4096, 0, NULL, 0), TAG, "UART");
    ESP_RETURN_ON_ERROR(uart_param_config(UART, &(uart_config_t){.baud_rate = 115200,
                                                                 .data_bits = UART_DATA_8_BITS,
                                                                 .parity = UART_PARITY_DISABLE,
                                                                 .stop_bits = UART_STOP_BITS_1,
                                                                 .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                                                                 .source_clk = UART_SCLK_DEFAULT}),
                        TAG, "UART");
    ESP_RETURN_ON_ERROR(uart_set_pin(UART, TX, RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "pins");
    // Without a module, silence rather than framing errors
    ESP_RETURN_ON_ERROR(gpio_pullup_en(RX), TAG, "pull-up");
    // Stack in PSRAM: a task there must never write flash
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCoreWithCaps(reader, "catm", 4096, NULL, 6, NULL, 1, MALLOC_CAP_SPIRAM) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "reader");
    open = true;
    return ESP_OK;
}

// Answering in command mode
static esp_err_t answering(void) {
    char line[LINE];
    if (lines == NULL || data_mode) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(open_uart(), TAG, "UART");
    if (atomic_exchange(&heard, false)) unanswered = -90 * SECOND;
    if (esp_timer_get_time() - unanswered < 90 * SECOND) {
        ESP_LOGW(TAG, "the modem left a command unanswered: wait 90 s before asking again");
        return ESP_ERR_INVALID_STATE;
    }
    uart_write_bytes(UART, terminate, sizeof terminate);
    for (int64_t deadline = esp_timer_get_time() + 2 * SECOND; receive(line, deadline) && strcmp(line, "NO CARRIER") != 0;) {}
    bool present = false;
    for (int i = 0; i < 3 && !(present = at("AT", "", NULL)); i++) {}
    if (!present) {
        ESP_LOGW(TAG, "no modem answered");
        return ESP_ERR_NOT_FOUND;
    }
    unanswered = -90 * SECOND;
    atomic_store(&answered, true);
    ask(MODEM_REVISION);
    return ESP_OK;
}

// Items first..last - 1, stopping at one left unanswered; false then
static bool read_items(modem_item first, modem_item last) {
    for (int i = first; i < (int)last; i++)
        if (ask((modem_item)i) < 0) return false;
    return true;
}

// Each write the settings call for, then the settings read again; a write left unanswered ends it
static esp_err_t keep_settings(void) {
    bool sent[MODEM_ITEMS] = {false}, any = false;
    for (int i = 0; i < MODEM_REVISION; i++) {
        if (!modem_wanted(&fresh, (modem_item)i)) continue;
        int64_t before = unanswered;
        bool ok = at(modem_commands[i].write, "", NULL);
        if (unanswered != before) return ESP_ERR_TIMEOUT;
        ESP_LOGW(TAG, "%s: %s", modem_commands[i].write, ok ? "OK" : "ERROR");
        sent[i] = any = true;
    }
    if (any && !read_items(0, MODEM_REVISION)) return ESP_ERR_TIMEOUT;
    // eDRX ends with the TAU that follows: CEDRXRDP may still show it
    for (int i = 0; i < MODEM_REVISION; i++)
        if (sent[i]) ESP_LOGW(TAG, "%s now: %s", modem_commands[i].name, fresh.values[i == MODEM_EDRX ? MODEM_EDRX_NETWORK : i]);
    return ESP_OK;
}

// Answering and ready to dial. After the checks, so that a slow read cannot hold them: the network each time, the settings
// when asked for, and once a boot with no back-off in sight, the settings read and kept
static esp_err_t ready(bool settings) {
    esp_err_t err = answering();
    if (err != ESP_OK) return err;
    err = check();
    bool keep = !kept && (err == ESP_OK || err == ESP_ERR_NOT_FINISHED);
    if (err != ESP_ERR_TIMEOUT && (!read_items(MODEM_SIGNAL, MODEM_ITEMS) || ((settings || keep) && !read_items(0, MODEM_REVISION))))
        err = ESP_ERR_TIMEOUT;
    if (keep && err != ESP_ERR_TIMEOUT) {
        kept = true;
        if (keep_settings() != ESP_OK) err = ESP_ERR_TIMEOUT;
    }
    taskENTER_CRITICAL(&guard);
    shown = fresh;
    taskEXIT_CRITICAL(&guard);
    return err;
}

bool catm_answered(void) {
    return atomic_load(&answered);
}

esp_err_t catm_status(void) {
    esp_netif_ip_info_t ip;
    if (lines == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!data_mode)
        err = ready(false);
    else if (esp_netif_get_ip_info(netif, &ip) == ESP_OK)
        ESP_LOGI(TAG, "PPP, ip " IPSTR, IP2STR(&ip.ip));
    xSemaphoreGive(lock);
    return err;
}

void catm_snapshot(modem_snapshot* out) {
    taskENTER_CRITICAL(&guard);
    *out = shown;
    taskEXIT_CRITICAL(&guard);
}

esp_err_t catm_read(void) {
    if (lines == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = data_mode ? ESP_ERR_INVALID_STATE : ready(true);
    xSemaphoreGive(lock);
    return err;
}

static void disconnect(void);

static esp_err_t dial(void) {
    char line[LINE];
    esp_err_t err = ready(false);
    if (err != ESP_OK) return err;
    // So that no late result of the checks reads as the dial's
    vTaskDelay(pdMS_TO_TICKS(1000));
    xEventGroupClearBits(events, GOT_IP | DEAD);
    dialing = true;
    send("ATD*99#");
    bool connected = false;
    int64_t deadline = esp_timer_get_time() + 30 * SECOND;
    while (!connected && receive(line, deadline) && strcmp(line, "NO CARRIER") != 0 && strcmp(line, "ERROR") != 0)
        connected = strncmp(line, "CONNECT", 7) == 0;
    dialing = false;
    if (!connected) {
        if (esp_timer_get_time() >= deadline) unanswered = esp_timer_get_time(), atomic_store(&heard, false);
        return ESP_FAIL;
    }
    esp_netif_action_start(netif, NULL, 0, NULL);
    if (xEventGroupWaitBits(events, GOT_IP | DEAD, pdFALSE, pdFALSE, pdMS_TO_TICKS(60000)) & GOT_IP) return ESP_OK;
    disconnect();
    return ESP_ERR_TIMEOUT;
}

esp_err_t catm_connect(void) {
    if (lines == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = dial();
    xSemaphoreGive(lock);
    return err;
}

static void disconnect(void) {
    char line[LINE];
    if (!data_mode) return;
    esp_netif_action_stop(netif, NULL, 0, NULL);
    if (!(xEventGroupWaitBits(events, DEAD, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000)) & DEAD)) {
        ESP_LOGW(TAG, "PPP did not end");
        return;
    }
    for (int64_t deadline = esp_timer_get_time() + 5 * SECOND; receive(line, deadline) && strcmp(line, "NO CARRIER") != 0;) {}
    if (!at("AT", "", NULL)) ESP_LOGW(TAG, "no answer after PPP");
}

void catm_disconnect(void) {
    if (lines == NULL) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    disconnect();
    xSemaphoreGive(lock);
}

bool catm_connected(void) {
    return events != NULL && (xEventGroupGetBits(events) & (GOT_IP | DEAD)) == GOT_IP;
}

esp_netif_t* catm_netif(void) {
    return netif;
}

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    (void)id;
    if (base == IP_EVENT) {
        ESP_LOGI(TAG, "ip " IPSTR, IP2STR(&((ip_event_got_ip_t*)data)->ip_info.ip));
        xEventGroupSetBits(events, GOT_IP);
    } else {
        // The modem's NO CARRIER and everything after it are lines again
        data_mode = false;
        xEventGroupSetBits(events, DEAD);
    }
}

static esp_err_t transmit(void* handle, void* buffer, size_t length) {
    (void)handle;
    uart_write_bytes(UART, buffer, length);
    return ESP_OK;
}

static esp_err_t attach(esp_netif_t* esp_netif, esp_netif_iodriver_handle handle) {
    ((esp_netif_driver_base_t*)handle)->netif = esp_netif;
    return esp_netif_set_driver_config(esp_netif, &(esp_netif_driver_ifconfig_t){.handle = handle, .transmit = transmit});
}

esp_err_t catm_start(void) {
    static esp_netif_driver_base_t driver = {.post_attach = attach};
    lines = xQueueCreate(16, LINE);
    lock = xSemaphoreCreateMutex();
    events = xEventGroupCreate();
    netif = esp_netif_new(&(esp_netif_config_t)ESP_NETIF_DEFAULT_PPP());
    ESP_RETURN_ON_FALSE(lines != NULL && lock != NULL && events != NULL && netif != NULL, ESP_ERR_NO_MEM, TAG, "objects");
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, &driver), TAG, "attach");
    // The modem asks for CHAP with an empty name
    ESP_RETURN_ON_ERROR(esp_netif_ppp_set_auth(netif, NETIF_PPP_AUTHTYPE_CHAP, "", ""), TAG, "auth");
    ESP_RETURN_ON_ERROR(esp_netif_ppp_set_params(netif, &(esp_netif_ppp_config_t){.ppp_phase_event_enabled = true}), TAG, "params");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, on_event, NULL), TAG, "events");
    return esp_event_handler_register(NETIF_PPP_STATUS, NETIF_PPP_PHASE_DEAD, on_event, NULL);
}
