// SPDX-License-Identifier: GPL-3.0-or-later
#include "network.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

#include "c6.h"
#include "c6_protocol.h"
#include "lte.h"
#include "probe.h"
#include "wifi.h"

#define READY BIT0

static const char* TAG = "network";
static const char* const path_names[] = {"none", "Wi-Fi", "LTE"};
static TaskHandle_t task;
static EventGroupHandle_t paths;
static network_choice choice;
static portMUX_TYPE choice_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool got_ip;

static esp_netif_t* netif_of(network_path p) {
    return p == NETWORK_WIFI ? wifi_netif() : p == NETWORK_LTE ? lte_netif() : NULL;
}

static void changed(void) {
    if (task != NULL) xTaskNotifyGive(task);
}

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg, (void)base, (void)id, (void)data;
    got_ip = true;
    changed();
}

// Each path's view at each change, and every second for the times in the rules
static void manage(void* arg) {
    (void)arg;
    network_path current = NETWORK_NONE;
    for (;;) {
        wifi_view w;
        lte_view l;
        wifi_view_now(&w);
        lte_view_now(&l);
        network_choice c = network_choose(&w, &l, current, esp_timer_get_time());
        // Published before READY, which sessions wait for
        taskENTER_CRITICAL(&choice_lock);
        choice = c;
        taskEXIT_CRITICAL(&choice_lock);
        if (c.path != current) {
            ESP_LOGI(TAG, "path %s", path_names[c.path]);
            lte_chosen(c.path == NETWORK_LTE);
            if (c.path != NETWORK_NONE)
                xEventGroupSetBits(paths, READY);
            else
                xEventGroupClearBits(paths, READY);
        }
        // A lease or redial keeps its DNS servers on its interface: only setting the default hands them to lwIP, for SNTP
        if (c.path != NETWORK_NONE && (atomic_exchange(&got_ip, false) || c.path != current)) esp_netif_set_default_netif(netif_of(c.path));
        current = c.path;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
}

esp_err_t network_start(const global_state* global) {
    paths = xEventGroupCreate();
    if (paths == NULL) return ESP_ERR_NO_MEM;
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, on_event, NULL);
    // Before the paths, whose changes it hears; stack in PSRAM: a task there must never write flash
    if (xTaskCreatePinnedToCoreWithCaps(manage, "network", 4096, NULL, 5, &task, 1, MALLOC_CAP_SPIRAM) != pdPASS) return ESP_ERR_NO_MEM;
    esp_err_t err = lte_start(global, changed);
    if (err != ESP_OK) ESP_LOGE(TAG, "no LTE: %s", esp_err_to_name(err));
    return wifi_start(global, changed);
}

static void network_now(network_choice* out) {
    taskENTER_CRITICAL(&choice_lock);
    *out = choice;
    taskEXIT_CRITICAL(&choice_lock);
}

bool network_wait_for(int timeout_ms) {
    return xEventGroupWaitBits(paths, READY, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms)) & READY;
}

static int try_ms(network_path p) {
    return p == NETWORK_LTE ? 4000 : 2000;
}

int network_timeout_ms(network_path path) {
    return path == NETWORK_LTE ? 30000 : 10000;
}

static network_lookup lookup(network_path p, const char* host, uint32_t* address) {
    probe_lookup l = probe_resolve(netif_of(p), host, try_ms(p), address);
    // Slept: once more, now held awake
    if (l == PROBE_SLEPT) l = probe_resolve(netif_of(p), host, try_ms(p), address);
    return l == PROBE_FOUND ? NETWORK_FOUND : l == PROBE_NO_NAME ? NETWORK_NOT_FOUND : NETWORK_NO_ANSWER;
}

// Unknown without a link: the internet is checked again at each
static network_health health(network_path p) {
    wifi_view w;
    lte_view l;
    if (p == NETWORK_WIFI) return wifi_view_now(&w), w.link ? w.internet : NETWORK_UNKNOWN;
    return lte_view_now(&l), l.state == LTE_UP ? l.internet : NETWORK_UNKNOWN;
}

static int64_t result_at(network_path p) {
    wifi_view w;
    lte_view l;
    return p == NETWORK_WIFI ? (wifi_view_now(&w), w.internet_at) : (lte_view_now(&l), l.internet_at);
}

static int64_t suspect(network_path p) {
    return p == NETWORK_WIFI ? wifi_suspect() : lte_suspect();
}

// The path's next verdict, until due
static network_health verdict(network_path p, int64_t due) {
    for (int64_t before = result_at(p); due != 0 && esp_timer_get_time() < due && result_at(p) == before;)
        vTaskDelay(pdMS_TO_TICKS(200));
    return health(p);
}

network_trouble network_find(const char* host, network_route* route) {
    network_choice c;
    wifi_view w;
    // While Wi-Fi rejoins, its DNS cannot answer: no name lookup to pin a host elsewhere meanwhile
    for (int i = 0; i < 100; i++) {
        network_now(&c);
        wifi_view_now(&w);
        if (c.path != NETWORK_WIFI || !wifi_rejoining(&w, esp_timer_get_time())) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // A host not found keeps the path it was looked up on: its session waits there, moving only with the path
    *route = (network_route){.use = {.path = c.path, .epoch = c.epoch[c.path]}};
    if (c.path == NETWORK_NONE) return NETWORK_NO_INTERNET;
    uint32_t here = 0, there = 0;
    network_lookup on_chosen = lookup(c.path, host, &here), on_other = NETWORK_NO_ANSWER;
    network_health reaches = health(c.path);
    // Its DNS silent: the path itself may have lost the internet; if not, its DNS is asked once more (a new link's may be slow)
    if (on_chosen == NETWORK_NO_ANSWER) {
        reaches = verdict(c.path, suspect(c.path));
        if (reaches == NETWORK_GOOD) on_chosen = lookup(c.path, host, &here);
    }
    if (network_ask_other(&c, on_chosen, reaches == NETWORK_GOOD)) on_other = lookup(network_other(c.path), host, &there);
    network_use use = network_host_path(&c, on_chosen, on_other);
    if (use.pinned) ESP_LOGI(TAG, "%s: only %s's DNS knows it", host, path_names[use.path]);
    if (!use.pinned && on_chosen != NETWORK_FOUND) return network_judge(false, reaches != NETWORK_BAD);
    *route = (network_route){.use = use, .address = use.pinned ? there : here};
    return NETWORK_FINE;
}

bool network_moved(const network_use* use) {
    network_choice c;
    network_now(&c);
    return network_session_moves(use, &c);
}

network_trouble network_after_drop(const ssh_host* host, const network_route* used, bool ran) {
    network_route r = *used;
    // A new path is worth trying at once
    if (network_moved(&used->use)) return NETWORK_FINE;
    // After a connection that ran: whether the host answers now, as a server that restarts drops its sessions
    network_trouble trouble = ran ? network_find(host->hostname, &r) : NETWORK_FINE;
    if (trouble != NETWORK_FINE) return trouble;
    int rc = ran ? probe_connect(netif_of(r.use.path), r.address, host->port, network_timeout_ms(r.use.path)) : ETIMEDOUT;
    // Refused: the host is there; a sleep inside: no verdict
    if (rc == 0 || rc == ECONNREFUSED || rc == EINTR) return NETWORK_FINE;
    int64_t due = suspect(r.use.path);
    char address[16];
    inet_ntoa_r(r.address, address, sizeof address);
    trouble = network_judge(true, verdict(r.use.path, due) != NETWORK_BAD);
    ESP_LOGW(TAG, "%s to %s (%s): %s", path_names[r.use.path], host->hostname, address, network_trouble_text(trouble));
    return trouble;
}

int network_bind(int fd, void* route) {
    return probe_bind(fd, netif_of(((const network_route*)route)->use.path));
}

void network_keepalive(int fd, network_path path) {
    // LTE: a probe every 6 s keeps the radio from idling (5-8 s gave the fewest stalls), and a link is dead only after
    // 120 s (30-90 s declared live links dead); Wi-Fi: dead 12-17 s after a drop, past the 10 s a rejoin may take
    bool lte = path == NETWORK_LTE;
    int idle = lte ? 6 : 5, interval = lte ? 6 : 2, count = lte ? 20 : 6;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &(int){1}, sizeof(int));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count);
}

static const char* health_name(network_health h) {
    return h == NETWORK_GOOD ? "good" : h == NETWORK_BAD ? "bad" : "checking";
}

static void address_of(esp_netif_t* netif, char out[16]) {
    esp_netif_ip_info_t ip = {0};
    if (netif != NULL) esp_netif_get_ip_info(netif, &ip);
    esp_ip4addr_ntoa(&ip.ip, out, 16);
}

static void ago(int64_t at, int64_t now, char out[24]) {
    if (at == 0)
        strlcpy(out, "never", 24);
    else
        snprintf(out, 24, "%lld s ago", (now - at) / NETWORK_SECOND);
}

static void status(void) {
    static const char* const c6_wifi_names[] = C6_WIFI_NAMES;
    network_choice c;
    wifi_view w;
    lte_view l;
    char wifi_address[16], lte_address[16], wifi_checked[24], lte_checked[24], c6[24];
    int64_t now = esp_timer_get_time();
    network_now(&c);
    wifi_view_now(&w);
    lte_view_now(&l);
    address_of(wifi_netif(), wifi_address);
    address_of(lte_netif(), lte_address);
    ago(w.internet_at, now, wifi_checked);
    ago(l.internet_at, now, lte_checked);
    c6_version(c6, sizeof c6);
    ESP_LOGI(TAG, "path %s; Wi-Fi %s, %s %s %s, internet %s (%s), epoch %u (C6 %s %u, %s); LTE %s %s, internet %s (%s), epoch %u", path_names[c.path],
             w.enabled ? "on" : "off", w.link ? "joined" : "not joined", w.ssid, wifi_address, health_name(w.internet), wifi_checked, w.epoch, c6,
             c6_protocol(), c6_wifi_names[w.c6_state % 7], l.state == LTE_UP ? "up" : "not up", lte_address, health_name(l.internet), lte_checked,
             l.epoch);
}

// net check HOST PORT [wifi|lte]: the path's DNS, then a TCP connection, through that path
static void check(const char* host, uint16_t port, const char* path) {
    network_choice c;
    network_now(&c);
    network_path p = strcmp(path, "wifi") == 0 ? NETWORK_WIFI : strcmp(path, "lte") == 0 ? NETWORK_LTE : c.path;
    esp_netif_t* netif = netif_of(p);
    esp_netif_dns_info_t primary = {0}, backup = {0};
    char local[16], address[16] = "?", servers[2][16];
    uint32_t found = 0;
    if (netif != NULL) esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &primary), esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &backup);
    address_of(netif, local);
    ESP_LOGI(TAG, "check over %s %s, DNS %s %s then 1.1.1.1", path_names[p], local, esp_ip4addr_ntoa(&primary.ip.u_addr.ip4, servers[0], 16),
             esp_ip4addr_ntoa(&backup.ip.u_addr.ip4, servers[1], 16));
    int64_t start = esp_timer_get_time();
    probe_lookup l = probe_resolve(netif, host, try_ms(p), &found);
    static const char* const lookups[] = {"found", "no such name", "no answer", "path down", "slept"};
    ESP_LOGI(TAG, "%s: %s after %lld ms", host, lookups[l], (esp_timer_get_time() - start) / 1000);
    if (l != PROBE_FOUND) return;
    inet_ntoa_r(found, address, sizeof address);
    start = esp_timer_get_time();
    int rc = probe_connect(netif, found, port, 20000);
    ESP_LOGI(TAG, "%s (%s) port %u: %s after %lld ms", host, address, port, rc == 0 ? "connected" : strerror(rc),
             (esp_timer_get_time() - start) / 1000);
}

void network_describe(network_info* out) {
    network_choice c;
    wifi_view w;
    lte_view l;
    wifi_ap_record_t ap = {0};
    network_now(&c);
    wifi_view_now(&w);
    lte_view_now(&l);
    *out = (network_info){.path = path_names[c.path],
                          .wifi_on = w.enabled,
                          .wifi_joined = w.link,
                          .wifi_internet = w.link ? w.internet : NETWORK_UNKNOWN,
                          .lte_on = l.state != LTE_OFF,
                          .lte = l.state,
                          .rssi = w.rssi};
    memcpy(out->ssid, w.ssid, sizeof out->ssid);
    if (w.link && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) out->rssi = ap.rssi;
    if (w.link) address_of(wifi_netif(), out->wifi_address);
    if (l.state == LTE_UP) address_of(lte_netif(), out->lte_address);
}

bool network_command(const char* line) {
    char host[64], path[8] = "";
    uint16_t port;
    if (strcmp(line, "wifi on") == 0 || strcmp(line, "wifi off") == 0) {
        wifi_turn(line[6] == 'n');
    } else if (strcmp(line, "lte on") == 0 || strcmp(line, "lte off") == 0) {
        lte_turn(line[5] == 'n');
    } else if (strcmp(line, "wifi drop") == 0) {
        wifi_drop();
    } else if (strcmp(line, "net status") == 0) {
        status();
    } else if (sscanf(line, "net check %63s %hu %7s", host, &port, path) >= 2) {
        check(host, port, path);
    } else if (strncmp(line, "net dns dead ", 13) == 0) {
        probe_dead_dns(strcmp(line + 13, "wifi") == 0 ? wifi_netif() : strcmp(line + 13, "lte") == 0 ? lte_netif() : NULL);
        ESP_LOGI(TAG, "DNS dead: %s", line + 13);
    } else {
        return lte_command(line);
    }
    return true;
}
