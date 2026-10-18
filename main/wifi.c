// SPDX-License-Identifier: GPL-3.0-or-later
#include "wifi.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_check.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif_net_stack.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <lwip/netif.h>

#include "c6.h"
#include "c6_protocol.h"
#include "sleep.h"

#define GOT_IP      BIT0
#define JOIN_MS     15000
#define ANSWER_MS   17000
#define MAX_VISIBLE 16

static const char* TAG = "wifi";
static const char* const c6_wifi_names[] = C6_WIFI_NAMES;
static const global_state* state;
static void (*changed)(void);
static TaskHandle_t task;
static EventGroupHandle_t events;
static esp_netif_t* netif;
// The C6's last lease, and whether the interface holds its address (default event loop only)
static c6_lease lease;
static bool held;
// Written by the default event loop (the C6's states and answers, leases, drops), wifi_turn and the task
static wifi_view view;
static portMUX_TYPE view_lock = portMUX_INITIALIZER_UNLOCKED;
// The ask the C6 has not answered yet, and the address and network of the last link (view_lock)
static uint8_t asked_sequence;
static bool waiting;
static uint32_t linked_address;
static char linked_ssid[33];
// Started: the station runs as on says; unusable: no Wi-Fi this boot. Neither yet: Wi-Fi is starting, a first try under way
static atomic_bool on, started, unusable, suspected, list_changed, dropping, scan_wanted;
// One join at a time, run by the task
static SemaphoreHandle_t join_lock, join_done;
static const wifi_network* _Atomic joining;
static esp_err_t join_error;
static _Atomic wifi_scan_state scan_state;
static wifi_visible_network visible[MAX_VISIBLE];
static size_t visible_count;
static portMUX_TYPE visible_lock = portMUX_INITIALIZER_UNLOCKED;

static void tell(void) {
    if (changed != NULL) changed();
}

static void wake(void) {
    if (task != NULL) xTaskNotifyGive(task);
}

// The link: the C6 joined, and the interface holds its lease
static void refresh(void) {
    c6_wifi_status c6 = {0};
    unsigned reports = c6_wifi_state(&c6);
    bool link = held && reports > 0 && c6.state == C6_WIFI_CONNECTED;
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&view_lock);
    view.c6_heard = reports > 0;
    view.c6_state = c6.state, view.rssi = c6.rssi;
    memcpy(view.ssid, c6.ssid, sizeof view.ssid);
    if (link && !view.link) {
        if (lease.address != linked_address || strcmp(c6.ssid, linked_ssid) != 0) view.epoch++;
        linked_address = lease.address;
        memcpy(linked_ssid, c6.ssid, sizeof linked_ssid);
        view.linked_at = now;
        view.internet = NETWORK_UNKNOWN;
    } else if (!link && view.link) {
        // Drops asked for are no flapping
        if (!atomic_exchange(&dropping, false) && !view.planned && atomic_load(&on)) view.failed_before = view.failed_at, view.failed_at = now;
        view.lost_at = now;
    }
    view.link = link;
    taskEXIT_CRITICAL(&view_lock);
    if (link)
        xEventGroupSetBits(events, GOT_IP);
    else
        xEventGroupClearBits(events, GOT_IP);
    tell();
    wake();
}

// The C6's own checks (sequence 0) and its answers; one begun before the link came is the network before
static void on_internet(uint8_t sequence, const c6_reach_answer* a) {
    int64_t now = esp_timer_get_time();
    bool counted = false, answered = false;
    taskENTER_CRITICAL(&view_lock);
    // Begun after the last drop: on this link, as a new link follows a drop, or the first
    if ((sequence == 0 || sequence == asked_sequence) && view.link && now - a->milliseconds * 1000LL >= view.lost_at && a->error != ENETUNREACH) {
        if (view.internet == NETWORK_GOOD && a->error != 0) view.failed_before = view.failed_at, view.failed_at = now;
        if (view.internet != NETWORK_GOOD && a->error == 0) view.good_since = now;
        view.internet = a->error == 0 ? NETWORK_GOOD : NETWORK_BAD;
        view.internet_at = now;
        counted = true;
    }
    if (waiting && (sequence == asked_sequence || counted)) waiting = false, answered = true;
    taskEXIT_CRITICAL(&view_lock);
    if (answered) sleep_hold(false);
    ESP_LOGI(TAG, "internet %s %s after %u ms%s%s", a->address, a->error == 0 ? "reached" : strerror(a->error), a->milliseconds,
             sequence == 0 ? " (the C6's check)" : "", counted ? "" : ": not for this link");
    if (counted) tell();
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

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected (%d)", ((wifi_event_sta_disconnected_t*)data)->reason);
        held = false;
        refresh();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        follow_lease(true);
    }
}

static esp_err_t link_down(void* ctx) {
    netif_set_link_down(esp_netif_get_netif_impl(ctx));
    return ESP_OK;
}

// With Network Split the interface keeps its address through a drop, and with it the TCP connections that a rejoin with the
// same lease carries on; esp_netif's own action would take the address
// NOLINTBEGIN(bugprone-reserved-identifier): --wrap names it
void __real_esp_netif_action_disconnected(void* esp_netif, esp_event_base_t base, int32_t id, void* data);

void __wrap_esp_netif_action_disconnected(void* esp_netif, esp_event_base_t base, int32_t id, void* data) {
    if (esp_netif == (netif != NULL ? netif : esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"))) {
        esp_netif_tcpip_exec(link_down, esp_netif);
        return;
    }
    __real_esp_netif_action_disconnected(esp_netif, base, id, data);
}
// NOLINTEND(bugprone-reserved-identifier)

// The C6 keeps its own copy of the known networks, with on for this boot
static void send_list(void) {
    uint8_t message[3 + C6_NETWORKS * (2 + sizeof state->wifi->ssid + sizeof state->wifi->password)];
    size_t size = 3;
    message[0] = P4_WIFI_LIST, message[1] = atomic_load(&on), message[2] = 0;
    for (const wifi_network* w = state->wifi; w != NULL && message[2] < C6_NETWORKS; w = w->next) {
        if (!w->ssid[0]) continue;
        message[size++] = (uint8_t)strlen(w->ssid);
        size += strlcpy((char*)message + size, w->ssid, sizeof w->ssid);
        message[size++] = (uint8_t)strlen(w->password);
        size += strlcpy((char*)message + size, w->password, sizeof w->password);
        message[2]++;
    }
    if (c6_send(message, size) != ESP_OK) ESP_LOGW(TAG, "known networks not sent to the C6");
    memset(message, 0, sizeof message);
}

// running: the station started; a start that failed is tried again by the task
static esp_err_t begin(bool* running) {
    events = xEventGroupCreate();
    join_lock = xSemaphoreCreateMutex();
    join_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(events != NULL && join_lock != NULL && join_done != NULL, ESP_ERR_NO_MEM, TAG, "objects");
    c6_on_wifi_state(refresh);
    c6_on_lease(on_lease);
    c6_on_internet(on_internet);
    ESP_RETURN_ON_ERROR(c6_start(), TAG, "C6");
    // Its protocol comes on its own, late from a C6 that boots afresh
    for (int i = 0; i < 30 && c6_protocol() == 0; i++)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (c6_protocol() < C6_INTERNET_SINCE) {
        ESP_LOGE(TAG, "C6 protocol %u: Wi-Fi needs %d, make c6-update", c6_protocol(), C6_INTERNET_SINCE);
        return ESP_ERR_NOT_SUPPORTED;
    }
    // As the tmux sessions' prefix
    uint8_t mac[6] = {0}, name[1 + C6_HOSTNAME];
    esp_efuse_mac_get_default(mac);
    name[0] = P4_HOSTNAME;
    int length = snprintf((char*)name + 1, sizeof name - 1, "tab5-%02x%02x%02x", mac[3], mac[4], mac[5]);
    if (c6_send(name, 1 + (size_t)length) != ESP_OK) ESP_LOGW(TAG, "hostname not sent to the C6");
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
    // it itself
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    if (atomic_load(&on) && !(*running = esp_wifi_start() == ESP_OK)) ESP_LOGW(TAG, "station not started");
    send_list();
    if (c6_send((const uint8_t[]){P4_READY}, 1) != ESP_OK) ESP_LOGW(TAG, "the C6 was not told this chip is ready");
    // Its lease may have come before the handlers
    if ((err = c6_ask_lease()) != ESP_OK) ESP_LOGW(TAG, "lease: %s", esp_err_to_name(err));
    return ESP_OK;
}

// Waits for the C6 to answer, once it stopped its own scan or join
static void hold_c6(bool hold) {
    unsigned before = c6_wifi_state(NULL);
    if (c6_send((const uint8_t[]){P4_WIFI_HOLD, hold}, 2) != ESP_OK) return;
    for (int i = 0; hold && i < 40 && c6_wifi_state(NULL) == before; i++)
        vTaskDelay(pdMS_TO_TICKS(50));
}

static int stronger(const void* a, const void* b) {
    return ((const wifi_visible_network*)b)->rssi - ((const wifi_visible_network*)a)->rssi;
}

// Each SSID once, at its strongest; hidden ones left out. Not while the C6 rejoins after a drop, which the hold would stop
static void scan(void) {
    wifi_ap_record_t* aps = NULL;
    uint16_t n = 0;
    wifi_visible_network found[MAX_VISIBLE] = {0};
    size_t count = 0;
    wifi_view v;
    for (wifi_view_now(&v); wifi_rejoining(&v, esp_timer_get_time()); wifi_view_now(&v))
        vTaskDelay(pdMS_TO_TICKS(200));
    hold_c6(true);
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_OK) esp_wifi_scan_get_ap_num(&n);
    if (err == ESP_OK && (aps = calloc(n + 1, sizeof *aps)) == NULL) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) esp_wifi_scan_get_ap_records(&n, aps);
    hold_c6(false);
    for (uint16_t i = 0; err == ESP_OK && i < n; i++) {
        size_t k = 0;
        while (k < count && strcmp(found[k].ssid, (const char*)aps[i].ssid) != 0)
            k++;
        if (aps[i].ssid[0] == 0 || (k == count && count == MAX_VISIBLE) || (k < count && aps[i].rssi <= found[k].rssi)) continue;
        if (k == count) count++;
        found[k] = (wifi_visible_network){.rssi = aps[i].rssi, .open = aps[i].authmode == WIFI_AUTH_OPEN};
        memcpy(found[k].ssid, aps[i].ssid, sizeof found[k].ssid - 1);
    }
    free(aps);
    qsort(found, count, sizeof found[0], stronger);
    taskENTER_CRITICAL(&visible_lock);
    memcpy(visible, found, count * sizeof found[0]);
    visible_count = count;
    taskEXIT_CRITICAL(&visible_lock);
    scan_state = err == ESP_OK ? WIFI_SCAN_DONE : WIFI_SCAN_FAILED;
    ESP_LOGI(TAG, "scan: %zu networks, %s", count, esp_err_to_name(err));
}

static void set_planned(bool planned, bool failed) {
    taskENTER_CRITICAL(&view_lock);
    view.planned = planned;
    // Back to the network before: rejoining as after a drop
    if (failed && !view.link) view.lost_at = esp_timer_get_time();
    taskEXIT_CRITICAL(&view_lock);
    tell();
}

// On first: the list then says so
static esp_err_t join(const wifi_network* network, bool* running) {
    uint8_t reason = 0;
    on = true;
    send_list();
    if (!*running) *running = esp_wifi_start() == ESP_OK;
    set_planned(true, false);
    // The C6 may wait for Wi-Fi to start and leave the network joined before
    esp_err_t err = c6_join(network->ssid, network->password, JOIN_MS + 6000, &reason);
    if (err == ESP_OK && !(xEventGroupWaitBits(events, GOT_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(JOIN_MS)) & GOT_IP)) err = ESP_ERR_TIMEOUT;
    set_planned(false, err != ESP_OK);
    ESP_LOGI(TAG, "%s joined by the C6: %s, reason %u", network->ssid, esp_err_to_name(err), reason);
    return err;
}

static bool is_waiting(void) {
    taskENTER_CRITICAL(&view_lock);
    bool was = waiting;
    taskEXIT_CRITICAL(&view_lock);
    return was;
}

// The ask waited for, if any, ends; so does its hold
static bool stop_waiting(void) {
    taskENTER_CRITICAL(&view_lock);
    bool was = waiting;
    waiting = false;
    taskEXIT_CRITICAL(&view_lock);
    if (was) sleep_hold(false);
    return was;
}

// Waiting before the ask: its answer may come before the send returns
static void ask(void) {
    static uint8_t sequence;
    // 1-255: 0 is the C6's own
    sequence = (uint8_t)(sequence % 255 + 1);
    suspected = false;
    sleep_hold(true);
    taskENTER_CRITICAL(&view_lock);
    int64_t before = view.asked_at;
    waiting = true;
    asked_sequence = sequence;
    view.asked_at = esp_timer_get_time();
    taskEXIT_CRITICAL(&view_lock);
    if (c6_ask_internet(sequence) == ESP_OK) return;
    // Not asked: asked again later
    stop_waiting();
    taskENTER_CRITICAL(&view_lock);
    view.asked_at = before;
    taskEXIT_CRITICAL(&view_lock);
}

// The asked check never came: the C6 restarted alone or hangs, and its network serves nothing
static void silent(void) {
    if (!stop_waiting()) return;
    taskENTER_CRITICAL(&view_lock);
    view.internet = NETWORK_BAD, view.internet_at = esp_timer_get_time();
    taskEXIT_CRITICAL(&view_lock);
    ESP_LOGW(TAG, "C6 silent: no answer in %d s", ANSWER_MS / 1000);
    tell();
}

static void run(void* arg) {
    (void)arg;
    int64_t start = esp_timer_get_time();
    taskENTER_CRITICAL(&view_lock);
    view.started_at = start;
    taskEXIT_CRITICAL(&view_lock);
    bool running = false, logged = false;
    started = begin(&running) == ESP_OK;
    unusable = !started;
    tell();
    for (;;) {
        wifi_view v;
        int64_t now = esp_timer_get_time(), at = NETWORK_NEVER;
        bool want = atomic_load(&on);
        if (started && want != running) {
            // The list says on before a start, off after a stop, for the C6 never to see itself started and off
            if (want) send_list();
            if ((want ? esp_wifi_start() : esp_wifi_stop()) == ESP_OK) running = want;
            if (!want) send_list();
        }
        if (started && atomic_exchange(&list_changed, false)) send_list();
        const wifi_network* network = atomic_exchange(&joining, NULL);
        if (network != NULL) {
            join_error = started ? join(network, &running) : ESP_ERR_NOT_SUPPORTED;
            xSemaphoreGive(join_done);
        }
        if (atomic_exchange(&scan_wanted, false)) {
            if (started && running)
                scan();
            else
                scan_state = WIFI_SCAN_FAILED;
        }
        wifi_view_now(&v);
        if (started && !logged && v.c6_heard && (v.link || (v.c6_state != C6_WIFI_SCANNING && v.c6_state != C6_WIFI_JOINING))) {
            logged = true;
            ESP_LOGI(TAG, "C6 first try: %s %s after %lld ms", c6_wifi_names[v.c6_state % 7], v.ssid, (now - start) / 1000);
        }
        bool pending = is_waiting();
        // An ask from before this link is no ask about it
        if (pending && v.asked_at < v.linked_at) pending = !stop_waiting();
        if (pending && wifi_c6_silent(&v, now)) silent();
        if (started && !pending) at = wifi_ask_at(&v, atomic_load(&suspected));
        if (at <= now) ask();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(at > now && at - now < NETWORK_SECOND ? (at - now) / 1000 + 1 : 1000));
    }
}

esp_err_t wifi_start(const global_state* global, void (*listener)(void)) {
    state = global;
    changed = listener;
    on = global->wifi_enabled;
    // Stack in PSRAM: a task there must never write flash
    return xTaskCreatePinnedToCoreWithCaps(run, "wifi", 6144, NULL, 5, &task, 1, MALLOC_CAP_SPIRAM) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void wifi_view_now(wifi_view* out) {
    taskENTER_CRITICAL(&view_lock);
    *out = view;
    taskEXIT_CRITICAL(&view_lock);
    out->enabled = atomic_load(&on) && !atomic_load(&unusable);
}

esp_netif_t* wifi_netif(void) {
    return netif;
}

void wifi_turn(bool turn_on) {
    on = turn_on;
    tell();
    wake();
}

void wifi_list_changed(void) {
    list_changed = true;
    wake();
}

esp_err_t wifi_join(const wifi_network* network) {
    if (!started || c6_protocol() < C6_JOIN_SINCE) return ESP_ERR_NOT_SUPPORTED;
    xSemaphoreTake(join_lock, portMAX_DELAY);
    xSemaphoreTake(join_done, 0);
    joining = network;
    wake();
    xSemaphoreTake(join_done, portMAX_DELAY);
    esp_err_t err = join_error;
    xSemaphoreGive(join_lock);
    return err;
}

int64_t wifi_suspect(void) {
    wifi_view v;
    wifi_view_now(&v);
    int64_t now = esp_timer_get_time();
    if (is_waiting()) return v.asked_at + ANSWER_MS * 1000LL;
    // Asked within 30 s: that verdict stands
    if (wifi_ask_at(&v, true) > now) return 0;
    suspected = true;
    wake();
    return now + ANSWER_MS * 1000LL;
}

void wifi_drop(void) {
    taskENTER_CRITICAL(&view_lock);
    // Counted at the link's end, which only a link has
    dropping = view.link;
    taskEXIT_CRITICAL(&view_lock);
    ESP_LOGI(TAG, "drop: %s", esp_err_to_name(esp_wifi_disconnect()));
}

void wifi_scan(void) {
    scan_state = WIFI_SCAN_RUNNING;
    scan_wanted = true;
    wake();
}

wifi_scan_state wifi_scan_now(void) {
    return scan_state;
}

bool wifi_visible(size_t index, wifi_visible_network* out) {
    taskENTER_CRITICAL(&visible_lock);
    bool found = index < visible_count;
    if (found) *out = visible[index];
    taskEXIT_CRITICAL(&visible_lock);
    return found;
}
