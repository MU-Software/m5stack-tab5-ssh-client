// SPDX-License-Identifier: GPL-3.0-or-later
#include "wifi.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <esp_check.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif_net_stack.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <lwip/netif.h>

#include "c6.h"
#include "c6_protocol.h"

#define GOT_IP BIT0
#define FAILED BIT1

static const char* TAG = "wifi";
static EventGroupHandle_t events;
static esp_netif_t* netif;
// Joined since the last join or stop: a disconnect is then retried rather than reported, unless the C6 rejoins by itself
static atomic_bool joined;
static void (*listener)(void);
// Its protocol is known: before, a join the C6 reports gets no DHCP here, and P4_READY or a join then replays it
static atomic_bool c6_known;
// Network Split: the C6's last lease, and whether the interface holds its address (default event loop only)
static c6_lease lease;
static atomic_bool held;

static bool split(void) {
    return c6_protocol() >= C6_NW_SPLIT_SINCE;
}

// esp_hosted makes it in its own task
static esp_netif_t* interface(void) {
    return netif != NULL ? netif : esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

// With Network Split: joined as the C6 says, with the address of its lease
static void refresh(void) {
    c6_wifi_status c6;
    if (!split()) return;
    if (held && c6_wifi_state(&c6) > 0 && c6.state == C6_WIFI_CONNECTED)
        xEventGroupSetBits(events, GOT_IP);
    else
        xEventGroupClearBits(events, GOT_IP);
}

static void follow_lease(bool announced) {
    esp_netif_ip_info_t ip;
    esp_netif_dns_info_t dns = {0};
    c6_lease now = {0};
    bool up = netif != NULL && esp_netif_is_netif_up(netif) && esp_netif_get_ip_info(netif, &ip) == ESP_OK;
    if (up && esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4)
        now.dns = dns.ip.u_addr.ip4.addr;
    if (up) now.address = ip.ip.addr, now.netmask = ip.netmask.addr, now.gateway = ip.gw.addr;
    bool was = held;
    c6_lease_step step = c6_lease_next(&lease, up ? &now : NULL, was, announced);
    if (step.set_dns) {
        dns = (esp_netif_dns_info_t){.ip = {.type = ESP_IPADDR_TYPE_V4, .u_addr.ip4.addr = lease.dns}};
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    ip = (esp_netif_ip_info_t){.ip.addr = lease.address, .netmask.addr = lease.netmask, .gw.addr = lease.gateway};
    if (step.set_netmask_gateway) esp_netif_set_ip_info(netif, &ip);
    held = step.held;
    if (step.held && (!was || step.set_netmask_gateway || step.set_dns)) {
        char address[16], gateway[16], server[16];
        esp_ip4_addr_t resolver = {lease.dns};
        ESP_LOGI(TAG, "ip %s, gateway %s, DNS %s from the C6", esp_ip4addr_ntoa(&ip.ip, address, sizeof address),
                 esp_ip4addr_ntoa(&ip.gw, gateway, sizeof gateway), esp_ip4addr_ntoa(&resolver, server, sizeof server));
    }
    if (!step.held && was) ESP_LOGI(TAG, "no lease from the C6: the address stays on the interface");
    refresh();
    if (step.announce) {
        ip_event_got_ip_t got = {.esp_netif = netif, .ip_info = ip};
        esp_event_post(IP_EVENT, IP_EVENT_STA_GOT_IP, &got, sizeof got, 0);
    }
}

static void on_lease(const c6_lease* now) {
    lease = *now;
    follow_lease(false);
}

static void on_c6_state(void) {
    refresh();
    if (listener != NULL) listener();
}

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected (%d)", ((wifi_event_sta_disconnected_t*)data)->reason);
        held = false;
        xEventGroupClearBits(events, GOT_IP);
        if (joined && c6_protocol() < 5)
            esp_wifi_connect();
        else
            xEventGroupSetBits(events, FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        joined = true;
        if (split()) {
            follow_lease(true);
            return;
        }
        ESP_LOGI(TAG, "ip " IPSTR, IP2STR(&((ip_event_got_ip_t*)data)->ip_info.ip));
        xEventGroupSetBits(events, GOT_IP);
    }
}

static esp_err_t link_down(void* ctx) {
    netif_set_link_down(esp_netif_get_netif_impl(ctx));
    return ESP_OK;
}

// esp_hosted's Network Split makes the interface without a DHCP client, which a C6 older than it leaves to this chip
// NOLINTBEGIN(bugprone-reserved-identifier): --wrap names them
void __real_esp_netif_action_connected(void* esp_netif, esp_event_base_t base, int32_t id, void* data);
void __real_esp_netif_action_disconnected(void* esp_netif, esp_event_base_t base, int32_t id, void* data);

void __wrap_esp_netif_action_connected(void* esp_netif, esp_event_base_t base, int32_t id, void* data) {
    __real_esp_netif_action_connected(esp_netif, base, id, data);
    if (esp_netif == interface() && c6_known && !split()) esp_netif_dhcpc_start(esp_netif);
}

// With Network Split the interface keeps its address through a drop, and with it the TCP connections that a rejoin with the
// same lease carries on; esp_netif's own action would take the address
void __wrap_esp_netif_action_disconnected(void* esp_netif, esp_event_base_t base, int32_t id, void* data) {
    if (esp_netif == interface() && split()) {
        esp_netif_tcpip_exec(link_down, esp_netif);
        return;
    }
    if (esp_netif == interface()) esp_netif_dhcpc_stop(esp_netif);
    __real_esp_netif_action_disconnected(esp_netif, base, id, data);
}
// NOLINTEND(bugprone-reserved-identifier)

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

esp_err_t wifi_start(bool on, void (*changed)(void)) {
    events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(events != NULL, ESP_ERR_NO_MEM, TAG, "events");
    listener = changed;
    c6_on_wifi_state(on_c6_state);
    c6_on_lease(on_lease);
    ESP_RETURN_ON_ERROR(c6_start(), TAG, "C6");
    c6_known = true;
    // As the tmux sessions' prefix
    uint8_t mac[6] = {0}, name[1 + C6_HOSTNAME];
    esp_efuse_mac_get_default(mac);
    name[0] = P4_HOSTNAME;
    int length = snprintf((char*)name + 1, sizeof name - 1, "tab5-%02x%02x%02x", mac[3], mac[4], mac[5]);
    if (split() && c6_send(name, 1 + (size_t)length) != ESP_OK) ESP_LOGW(TAG, "hostname not sent to the C6");
    // Network Split makes the interface in esp_hosted's own task, registering esp_wifi_stop last, which would stop the C6's Wi-Fi
    // at each restart of this chip
    bool unregistered = false;
    for (int tries = 0; tries < 300 && !unregistered; tries++) {
        unregistered =
            esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") != NULL && esp_unregister_shutdown_handler((shutdown_handler_t)esp_wifi_stop) == ESP_OK;
        if (!unregistered) vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_RETURN_ON_FALSE((netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) != NULL, ESP_FAIL, TAG, "no interface");
    if (!unregistered) ESP_LOGW(TAG, "esp_wifi_stop still runs at restarts");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    // A C6 booting afresh starts its own Wi-Fi meanwhile, which refuses this one until it is done
    esp_err_t err;
    for (int tries = 0; (err = esp_wifi_init(&init)) != ESP_OK && tries < 20; tries++)
        vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(err, TAG, "init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL), TAG, "events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "events");
    // The default flash storage would keep the password in the C6's NVS; esp_hosted's host drops it, so the C6 firmware sets
    // it itself from protocol 5
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    esp_err_t started = on ? esp_wifi_start() : ESP_OK;
    // Its lease may have come before the handlers
    if (split() && (err = c6_ask_lease()) != ESP_OK) ESP_LOGW(TAG, "lease: %s", esp_err_to_name(err));
    return started;
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
                if (esp_wifi_connect() == ESP_OK && (!split() || c6_ask_lease() == ESP_OK) &&
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

esp_err_t wifi_join_through_c6(const wifi_network* network, int timeout_ms) {
    uint8_t reason = 0;
    // The C6 may wait for Wi-Fi to start and leave the network joined before
    esp_err_t err = c6_join(network->ssid, network->password, timeout_ms + 6000, &reason);
    if (err == ESP_OK && !(xEventGroupWaitBits(events, GOT_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms)) & GOT_IP)) err = ESP_ERR_TIMEOUT;
    ESP_LOGI(TAG, "%s joined by the C6: %s, reason %u", network->ssid, esp_err_to_name(err), reason);
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
