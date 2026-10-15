// SPDX-License-Identifier: GPL-3.0-or-later
#include "catm.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <driver/gpio.h>
#include <driver/uart.h>
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

#define UART    UART_NUM_2
#define TX      6
#define RX      46
#define LINE    128
#define SECOND  1000000LL
#define GOT_IP  BIT0
#define DEAD    BIT1
#define NV_READ "AT+CNVR=1,\"/nv/item_files/modem/nas/mm_backoff_remaining_info\""

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
static bool answered; // this boot

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
            ESP_LOGI(TAG, "< %s", line);
            if (dialing && strncmp(line, "CONNECT", 7) == 0) data_mode = true;
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
    unanswered = esp_timer_get_time();
    return false;
}

// The item's first 16 hex digits are the T3346 back-off and timer 20 left
static bool no_backoff(const char* reply) {
    for (const char* p = reply; *p; p++) {
        size_t n = strspn(p, "0123456789abcdefABCDEF");
        if (n == 32) return strspn(p, "0") >= 16;
        p += n > 0 ? n - 1 : 0;
    }
    return false;
}

// Present, registered, and with the default bearer up, as the research bench required before every dial
static esp_err_t check(void) {
    char ctrj[LINE], nv[LINE], cereg[LINE], cpsi[LINE], cgact[LINE];
    long trj = -1;
    int stat = -1;
    if (!at("AT+CTRJ", "+CTRJ:", ctrj) || !at(NV_READ, "+CNVR:", nv) || !at("AT+CEREG?", "+CEREG:", cereg) || !at("AT+CPSI?", "+CPSI:", cpsi) ||
        !at("AT+CGACT?", "+CGACT: 1,", cgact))
        return ESP_ERR_TIMEOUT;
    sscanf(ctrj, "+CTRJ: %ld", &trj);
    sscanf(cereg, "+CEREG: %*d,%d", &stat);
    if (trj != 0 || !no_backoff(nv)) {
        ESP_LOGE(TAG, "a network back-off may be running: no dial");
        return ESP_ERR_INVALID_STATE;
    }
    // The short form stays for ~20 s after the radio is lost
    if ((stat != 1 && stat != 5) || strncmp(cpsi, "+CPSI: LTE CAT-M1,Online,450-", 29) != 0 || strcmp(cgact, "+CGACT: 1,1") != 0) {
        ESP_LOGW(TAG, "not registered with the default bearer up yet");
        return ESP_ERR_INVALID_STATE;
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
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(reader, "catm", 4096, NULL, 6, NULL, 1) == pdPASS, ESP_ERR_NO_MEM, TAG, "reader");
    open = true;
    return ESP_OK;
}

// Answering in command mode and ready to dial
static esp_err_t ready(void) {
    char line[LINE];
    if (lines == NULL || data_mode) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(open_uart(), TAG, "UART");
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
    answered = true;
    at("AT+CGMR", "", NULL);
    return check();
}

bool catm_answered(void) {
    return answered;
}

esp_err_t catm_status(void) {
    esp_netif_ip_info_t ip;
    if (lines == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!data_mode)
        err = ready();
    else if (esp_netif_get_ip_info(netif, &ip) == ESP_OK)
        ESP_LOGI(TAG, "PPP, ip " IPSTR, IP2STR(&ip.ip));
    xSemaphoreGive(lock);
    return err;
}

static void disconnect(void);

static esp_err_t dial(void) {
    char line[LINE];
    esp_err_t err = ready();
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
        if (esp_timer_get_time() >= deadline) unanswered = esp_timer_get_time();
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
