// SPDX-License-Identifier: GPL-3.0-or-later
#include "wifi.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <esp_check.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include "c6.h"

#define GOT_IP BIT0
#define FAILED BIT1

static const char* TAG = "wifi";
static EventGroupHandle_t events;
static esp_netif_t* netif;
// Joined since the last join or stop: a disconnect is then retried rather than reported, unless the C6 rejoins by itself
static atomic_bool joined;

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected (%d)", ((wifi_event_sta_disconnected_t*)data)->reason);
        xEventGroupClearBits(events, GOT_IP);
        if (joined && c6_protocol() < 5)
            esp_wifi_connect();
        else
            xEventGroupSetBits(events, FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "ip " IPSTR, IP2STR(&((ip_event_got_ip_t*)data)->ip_info.ip));
        joined = true;
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

esp_err_t wifi_start(bool on) {
    events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(events != NULL, ESP_ERR_NO_MEM, TAG, "events");
    // Before the C6 answers: one that started Wi-Fi by itself says so once, and esp_hosted passes on only the first
    netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_ERROR(c6_start(), TAG, "C6");
    // Registered with it, it would stop the C6's Wi-Fi at each restart of this chip
    if (esp_unregister_shutdown_handler((shutdown_handler_t)esp_wifi_stop) != ESP_OK) ESP_LOGW(TAG, "esp_wifi_stop still runs at restarts");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL), TAG, "events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "events");
    // The default flash storage would keep the password in the C6's NVS; esp_hosted's host drops it, so the C6 firmware sets
    // it itself from protocol 5
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    return on ? esp_wifi_start() : ESP_OK;
}

esp_err_t wifi_scan(wifi_ap_record_t** records, uint16_t* count) {
    // A network out of reach is retried at each disconnect, and the scan fails while it is; the C6's own tries stop for the
    // scan (P4_WIFI_HOLD)
    if (!(xEventGroupGetBits(events) & GOT_IP) && c6_protocol() < 5) {
        joined = false;
        esp_wifi_disconnect();
    }
    *records = NULL, *count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(NULL, true), TAG, "scan");
    esp_wifi_scan_get_ap_num(count);
    *records = calloc(*count + 1, sizeof **records);
    ESP_RETURN_ON_FALSE(*records != NULL, ESP_ERR_NO_MEM, TAG, "scan results");
    esp_wifi_scan_get_ap_records(count, *records);
    return ESP_OK;
}

esp_err_t wifi_join(const wifi_network* networks, const char* except, int timeout_ms) {
    wifi_ap_record_t ap;
    // Joined by the C6 through this chip's restart: connecting again replays the join, for DHCP
    if (!(xEventGroupGetBits(events) & GOT_IP) && except == NULL && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        for (const wifi_network* w = networks; w != NULL; w = w->next)
            if (strcmp(w->ssid, (const char*)ap.ssid) == 0) {
                ESP_LOGI(TAG, "joined %s already", w->ssid);
                xEventGroupClearBits(events, FAILED);
                if (esp_wifi_connect() == ESP_OK &&
                    (xEventGroupWaitBits(events, GOT_IP | FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms)) & GOT_IP))
                    return ESP_OK;
                break;
            }
    }
    wifi_ap_record_t* aps;
    uint16_t n;
    esp_err_t err = wifi_scan(&aps, &n);
    if (err != ESP_OK) return err;
    err = ESP_ERR_NOT_FOUND;
    for (const wifi_network* w = networks; w != NULL && err != ESP_OK; w = w->next) {
        if (!w->ssid[0] || (except != NULL && strcmp(w->ssid, except) == 0) || !seen(w->ssid, aps, n)) continue;
        // Leaves the network joined before only for one that is there
        if (xEventGroupGetBits(events) & GOT_IP) {
            joined = false;
            xEventGroupClearBits(events, FAILED);
            esp_wifi_disconnect();
            xEventGroupWaitBits(events, FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(2000));
        }
        joined = false;
        ESP_LOGI(TAG, "joining %s", w->ssid);
        err = join(w->ssid, w->password, timeout_ms);
    }
    free(aps);
    return err;
}

esp_err_t wifi_on(void) {
    return esp_wifi_start();
}

esp_err_t wifi_off(void) {
    joined = false;
    xEventGroupClearBits(events, GOT_IP);
    return esp_wifi_stop();
}

bool wifi_connected(void) {
    return events != NULL && (xEventGroupGetBits(events) & GOT_IP);
}

esp_netif_t* wifi_netif(void) {
    return netif;
}
