// SPDX-License-Identifier: GPL-3.0-or-later
#include "wifi.h"

#include <stdlib.h>
#include <string.h>

#include <esp_check.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include "c6.h"

#define GOT_IP BIT0
#define FAILED BIT1

static const char* TAG = "wifi";
static EventGroupHandle_t events;

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected (%d)", ((wifi_event_sta_disconnected_t*)data)->reason);
        if (xEventGroupGetBits(events) & GOT_IP)
            esp_wifi_connect();
        else
            xEventGroupSetBits(events, FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "ip " IPSTR, IP2STR(&((ip_event_got_ip_t*)data)->ip_info.ip));
        xEventGroupSetBits(events, GOT_IP);
    }
}

static int seen(const char* ssid, const wifi_ap_record_t* aps, uint16_t n) {
    for (uint16_t i = 0; i < n; i++)
        if (strcmp(ssid, (const char*)aps[i].ssid) == 0) return 1;
    return 0;
}

static esp_err_t join(const char* ssid, const char* password, int timeout_ms) {
    wifi_config_t cfg = {.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN};
    strlcpy((char*)cfg.sta.ssid, ssid, sizeof cfg.sta.ssid + 1);
    strlcpy((char*)cfg.sta.password, password, sizeof cfg.sta.password + 1);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(&cfg, 0, sizeof cfg);
    xEventGroupClearBits(events, FAILED);
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) return err;
    EventBits_t bits = xEventGroupWaitBits(events, GOT_IP | FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & GOT_IP) return ESP_OK;
    if (!(bits & FAILED)) {
        esp_wifi_disconnect();
        xEventGroupWaitBits(events, FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
    }
    return ESP_FAIL;
}

esp_err_t wifi_connect(const wifi_network* networks, int timeout_ms) {
    events = xEventGroupCreate();
    ESP_RETURN_ON_ERROR(c6_start(), TAG, "C6");
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "events");
    // The default flash storage would keep the password in the C6's NVS
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(NULL, true), TAG, "scan");
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t* aps = calloc(n + 1, sizeof *aps);
    ESP_RETURN_ON_FALSE(aps != NULL, ESP_ERR_NO_MEM, TAG, "scan results");
    esp_wifi_scan_get_ap_records(&n, aps);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (const wifi_network* w = networks; w != NULL && err != ESP_OK; w = w->next) {
        if (!seen(w->ssid, aps, n)) continue;
        ESP_LOGI(TAG, "joining %s", w->ssid);
        err = join(w->ssid, w->password, timeout_ms);
    }
    free(aps);
    // A started station stays the default interface, ahead of PPP
    if (err != ESP_OK) esp_wifi_stop();
    return err;
}
