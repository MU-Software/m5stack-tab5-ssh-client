// SPDX-License-Identifier: GPL-3.0-or-later
#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/poll.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_ppp.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <lwip/dns.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include "c6.h"
#include "catm.h"
#include "wifi.h"

#define SECOND 1000000LL
// A brief loss of the access point usually keeps the address and the TCP connection
#define WIFI_GRACE (3 * SECOND)
// Back from LTE only to a network that has stayed
#define WIFI_STABLE  (10 * SECOND)
#define WIFI_RECHECK (30 * SECOND)
#define WIFI_REJOIN  (60 * SECOND)
#define CHECK_MS     5000
#define JOIN_MS      15000
#define READY        BIT0

typedef enum { PATH_NONE, PATH_WIFI, PATH_LTE } path;

static const char* TAG = "net";
static const char* const path_names[] = {"none", "Wi-Fi", "LTE"};
static const global_state* state;
static TaskHandle_t manager, lte;
static EventGroupHandle_t paths;
// Joins and internet checks, from the manager or a command
static SemaphoreHandle_t lock, suspected;
static atomic_uint generation;
static atomic_int current;
static atomic_bool wifi_enabled = true, lte_enabled = true, lte_reset, suspect, wifi_started;
// The manager's view of Wi-Fi: since when it has an address, whether the internet check passed
static int64_t wifi_since, wifi_lost, wifi_checked, wifi_tried;
static bool wifi_ok;
static int wifi_failures;

static int64_t since(int64_t start) {
    return (esp_timer_get_time() - start) / 1000;
}

// A TCP connection to host:port, from local when it is not NULL; 0 or an errno
static int reach(const char* host, uint16_t port, const esp_ip4_addr_t* local, int timeout_ms, char address[16]) {
    char service[6];
    snprintf(service, sizeof service, "%u", port);
    struct addrinfo* ai;
    if (getaddrinfo(host, service, &(struct addrinfo){.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, &ai) != 0) return EHOSTUNREACH;
    if (address != NULL) strlcpy(address, inet_ntoa(((struct sockaddr_in*)ai->ai_addr)->sin_addr), 16);
    int rc = 0, fd = socket(AF_INET, SOCK_STREAM, 0);
    socklen_t length = sizeof rc;
    // The source address picks the interface (lwIP's IP4_ROUTE_SRC hook)
    struct sockaddr_in source = {.sin_family = AF_INET, .sin_addr.s_addr = local != NULL ? local->addr : 0};
    if (fd < 0 || (local != NULL && bind(fd, (struct sockaddr*)&source, sizeof source) != 0) || fcntl(fd, F_SETFL, O_NONBLOCK) != 0)
        rc = errno;
    else if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0 && (rc = errno) == EINPROGRESS) {
        if (poll(&(struct pollfd){.fd = fd, .events = POLLOUT}, 1, timeout_ms) != 1)
            rc = ETIMEDOUT;
        else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &rc, &length) != 0)
            rc = errno;
    }
    if (fd >= 0) close(fd);
    freeaddrinfo(ai);
    return rc;
}

void net_check(const char* host, uint16_t port) {
    esp_netif_t* netif = esp_netif_get_default_netif();
    esp_netif_ip_info_t ip = {0};
    char local[16], address[16] = "?";
    if (netif != NULL) esp_netif_get_ip_info(netif, &ip);
    esp_ip4addr_ntoa(&ip.ip, local, sizeof local);
    ESP_LOGI(TAG, "default %s %s, DNS %s %s", netif != NULL ? esp_netif_get_desc(netif) : "none", local, ipaddr_ntoa(dns_getserver(0)),
             ipaddr_ntoa(dns_getserver(1)));
    int64_t start = esp_timer_get_time();
    int rc = reach(host, port, NULL, 20000, address);
    ESP_LOGI(TAG, "%s (%s) port %u: %s after %lld ms", host, address, port, rc == 0 ? "connected" : strerror(rc), since(start));
}

// The SSH host through Wi-Fi: it, not just any site, is what the session needs. The manager does not wait for a join
// that a command started: it would hold every switch of path meanwhile. 1, 0, or -1 when busy
static int wifi_reaches_host(TickType_t wait) {
    const ssh_host* h = state->current_host;
    esp_netif_ip_info_t ip;
    if (h == NULL) return 1;
    if (esp_netif_get_ip_info(wifi_netif(), &ip) != ESP_OK) return 0;
    if (xSemaphoreTake(lock, wait) != pdTRUE) return -1;
    int64_t start = esp_timer_get_time();
    int rc = reach(h->hostname, h->port, &ip.ip, CHECK_MS, NULL);
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "Wi-Fi to %s: %s after %lld ms", h->hostname, rc == 0 ? "ok" : strerror(rc), since(start));
    return rc == 0;
}

static esp_netif_t* netif_of(path p) {
    return p == PATH_WIFI ? wifi_netif() : p == PATH_LTE ? catm_netif() : NULL;
}

static void choose(path want) {
    if (want == current) return;
    if (want != PATH_NONE) esp_netif_set_default_netif(netif_of(want));
    current = want;
    generation++;
    ESP_LOGI(TAG, "path %s", path_names[want]);
    if (want != PATH_NONE)
        xEventGroupSetBits(paths, READY);
    else
        xEventGroupClearBits(paths, READY);
}

// Another known network than except, or any when except is NULL
static void join_known(const char* except, TickType_t wait) {
    if (state->wifi == NULL || xSemaphoreTake(lock, wait) != pdTRUE) return;
    // A command may have joined meanwhile
    if (wifi_enabled && (except != NULL || !wifi_connected())) wifi_join(state->wifi, except, JOIN_MS);
    wifi_tried = esp_timer_get_time();
    xSemaphoreGive(lock);
}

static void manage(void* arg) {
    (void)arg;
    wifi_started = wifi_start() == ESP_OK;
    if (!wifi_started) ESP_LOGE(TAG, "no Wi-Fi");
    // LTE waits for the first try, so a boot does not connect over LTE only to move to Wi-Fi
    if (wifi_started) join_known(NULL, portMAX_DELAY);
    bool was_up = false;
    for (;;) {
        int64_t now = esp_timer_get_time();
        bool up = wifi_started && wifi_enabled && wifi_connected();
        if (up && !was_up) {
            wifi_since = now;
            wifi_checked = 0;
            wifi_ok = false;
            wifi_failures = 0;
        }
        if (!up && was_up) wifi_lost = now;
        was_up = up;
        if (wifi_started && wifi_enabled && !up && now - wifi_tried >= WIFI_REJOIN) join_known(NULL, 0);
        if (suspect) {
            int reached = up && current == PATH_WIFI ? wifi_reaches_host(pdMS_TO_TICKS(CHECK_MS)) : -1;
            if (reached >= 0) {
                wifi_ok = reached;
                wifi_checked = esp_timer_get_time();
            }
            suspect = false;
            xSemaphoreGive(suspected);
        }
        if (up && !wifi_ok && now - wifi_since >= (current == PATH_LTE ? WIFI_STABLE : 0) &&
            (wifi_checked == 0 || now - wifi_checked >= WIFI_RECHECK)) {
            int reached = wifi_reaches_host(0);
            if (reached >= 0) {
                wifi_ok = reached;
                wifi_checked = esp_timer_get_time();
            }
            // Joined but not getting through twice: another known network, if a scan sees one
            if (reached == 0 && ++wifi_failures >= 2) {
                wifi_ap_record_t ap = {0};
                wifi_failures = 0;
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) join_known((const char*)ap.ssid, 0);
                // A new network is checked as one, even when the switch fell between two looks
                was_up = false;
            }
        }
        path want = up && wifi_ok ? PATH_WIFI : catm_connected() ? PATH_LTE : PATH_NONE;
        if (!(current == PATH_WIFI && wifi_enabled && !up && now - wifi_lost < WIFI_GRACE)) choose(want);
        if (current != PATH_NONE && esp_netif_get_default_netif() != netif_of(current)) {
            ESP_LOGW(TAG, "default interface was changed: %s again", path_names[current]);
            esp_netif_set_default_netif(netif_of(current));
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
}

// Rules from the Stamp CAT-M research: the gate is read-only, so not-yet-registered retries every 90 s; a lost link waits
// 120 s doubling to 960 s, six times; two dials refused before CONNECT, or no modem, stop LTE until lte on
static void run_lte(void* arg) {
    (void)arg;
    int drops = 0, refusals = 0;
    for (;;) {
        if (lte_reset) {
            lte_reset = false;
            drops = refusals = 0;
        }
        if (!lte_enabled) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        esp_err_t err = catm_connect();
        xTaskNotifyGive(manager);
        int64_t wait = 90 * SECOND;
        if (err == ESP_OK) {
            refusals = 0;
            int64_t up = esp_timer_get_time();
            while (lte_enabled && catm_connected())
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            if (!lte_enabled) {
                catm_disconnect();
                continue;
            }
            if (esp_timer_get_time() - up >= 960 * SECOND) drops = 0;
            wait = (120 * SECOND) << (drops < 3 ? drops : 3);
            if (++drops > 6) lte_enabled = false;
        } else if (err == ESP_ERR_NOT_FOUND || (err == ESP_FAIL && ++refusals >= 2)) {
            lte_enabled = false;
        }
        if (!lte_enabled) {
            ESP_LOGW(TAG, "LTE off until lte on");
            continue;
        }
        ESP_LOGI(TAG, "LTE again in %lld s", wait / SECOND);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait / 1000));
    }
}

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    if (manager != NULL) xTaskNotifyGive(manager);
    if (lte != NULL) xTaskNotifyGive(lte);
}

esp_err_t net_start(const global_state* global) {
    state = global;
    paths = xEventGroupCreate();
    lock = xSemaphoreCreateMutex();
    suspected = xSemaphoreCreateBinary();
    if (paths == NULL || lock == NULL || suspected == NULL) return ESP_ERR_NO_MEM;
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, on_event, NULL);
    esp_event_handler_register(NETIF_PPP_STATUS, NETIF_PPP_PHASE_DEAD, on_event, NULL);
    if (xTaskCreatePinnedToCore(manage, "net", 6144, NULL, 5, &manager, 1) != pdPASS ||
        xTaskCreatePinnedToCore(run_lte, "lte", 4096, NULL, 5, &lte, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

unsigned net_wait(void) {
    xEventGroupWaitBits(paths, READY, pdFALSE, pdFALSE, portMAX_DELAY);
    return generation;
}

unsigned net_generation(void) {
    return generation;
}

void net_suspect(void) {
    suspect = true;
    xTaskNotifyGive(manager);
    xSemaphoreTake(suspected, pdMS_TO_TICKS(CHECK_MS * 3));
}

uint32_t net_local_address(void) {
    esp_netif_ip_info_t ip = {0};
    esp_netif_t* netif = netif_of(current);
    return netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK ? ip.ip.addr : 0;
}

void net_keepalive(int fd) {
    // LTE: a probe every 6 s keeps the radio from idling (5-8 s gave the fewest stalls), and a link is dead only after
    // 120 s (30-90 s declared live links dead); Wi-Fi: dead after ~11 s
    bool lte_path = current == PATH_LTE;
    int idle = lte_path ? 6 : 5, interval = lte_path ? 6 : 2, count = lte_path ? 20 : 3;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &(int){1}, sizeof(int));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count);
}

esp_err_t net_join(const wifi_network* network) {
    if (!wifi_started) return ESP_ERR_INVALID_STATE;
    if (!wifi_enabled) {
        wifi_enabled = true;
        wifi_on();
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = wifi_join(network, NULL, JOIN_MS);
    wifi_tried = esp_timer_get_time();
    xSemaphoreGive(lock);
    xTaskNotifyGive(manager);
    return err;
}

static void status(void) {
    esp_netif_ip_info_t ip = {0};
    wifi_ap_record_t ap = {0};
    char address[16], c6[24] = "off";
    if (wifi_connected()) {
        esp_wifi_sta_get_ap_info(&ap);
        esp_netif_get_ip_info(wifi_netif(), &ip);
    }
    esp_ip4addr_ntoa(&ip.ip, address, sizeof address);
    if (wifi_started) c6_version(c6, sizeof c6);
    ESP_LOGI(TAG, "path %s (%u); Wi-Fi %s %s %s %s (C6 %s); LTE %s %s", path_names[current], generation, wifi_enabled ? "on" : "off",
             (const char*)ap.ssid, address, wifi_connected() ? (wifi_ok ? "reaches the host" : "not reaching the host yet") : "not joined", c6,
             lte_enabled ? "on" : "off", catm_connected() ? "connected" : "not connected");
}

bool net_command(const char* line) {
    char host[64];
    uint16_t port;
    if (strcmp(line, "wifi on") == 0 || strcmp(line, "wifi off") == 0) {
        wifi_enabled = line[6] == 'n';
        if (wifi_started) wifi_enabled ? wifi_on() : wifi_off();
        wifi_tried = 0;
        xTaskNotifyGive(manager);
    } else if (strcmp(line, "lte on") == 0 || strcmp(line, "lte off") == 0) {
        lte_enabled = line[5] == 'n';
        lte_reset = true;
        xTaskNotifyGive(lte);
    } else if (strcmp(line, "lte status") == 0) {
        ESP_LOGI(TAG, "lte status: %s", esp_err_to_name(catm_status()));
    } else if (strcmp(line, "net status") == 0) {
        status();
    } else if (sscanf(line, "net check %63s %hu", host, &port) == 2) {
        net_check(host, port);
    } else {
        return false;
    }
    return true;
}
