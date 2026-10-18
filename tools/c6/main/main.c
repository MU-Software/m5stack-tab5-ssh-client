// SPDX-License-Identifier: Apache-2.0
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/poll.h>

#include <driver/sdio_slave.h>
#include <eh_cp_core.h>
#include <eh_cp_feat_nw_split.h>
#include <eh_cp_feat_peer_data.h>
#include <eh_cp_feat_wifi.h>
#include <eh_transport_cp.h>
#include <esp_attr.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "c6_nvs_server.h"
#include "c6_pick.h"
#include "c6_protocol.h"

#if !EH_CP_FEAT_NW_SPLIT_READY
#error "C6_NW_SPLIT_SINCE: the P4 takes this protocol for Network Split (sdkconfig.defaults; an old build/c6/sdkconfig?)"
#endif

#define RING 4096

static const char* TAG = "tab5";
// What this chip logged, sent on to the P4 once it reached us; the oldest goes when full
static char ring[RING];
static size_t head, used;
static portMUX_TYPE ring_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t uart;
static TaskHandle_t sender;
static volatile bool p4_reached;
// The logs, the answers to the P4 and the joining task all send
static SemaphoreHandle_t send_lock;

static esp_err_t send_to_p4(const uint8_t* message, size_t size) {
    xSemaphoreTake(send_lock, portMAX_DELAY);
    esp_err_t err = eh_cp_feat_peer_data_send(C6_TO_P4, message, size);
    xSemaphoreGive(send_lock);
    return err;
}

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
        if (n > 0 && send_to_p4(message, n + 1) != ESP_OK) p4_reached = false;
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

// The driver's credits for its send queue. Its reset hands a send in flight back to its sender but not the credit (in
// send_flush_data the step that ends the flush returns the descriptors without counting them): one lost for each restart of
// the P4 in the middle of a transfer, and once all are gone the next send waits for good
static SemaphoreHandle_t send_credits;
static UBaseType_t send_queue_size;

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
            // Nothing is queued now, and the senders run below this task
            while (send_credits != NULL && xSemaphoreGive(send_credits) == pdTRUE) {}
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
QueueHandle_t __real_xQueueCreateCountingSemaphore(UBaseType_t max, UBaseType_t initial);
QueueHandle_t __wrap_xQueueCreateCountingSemaphore(UBaseType_t max, UBaseType_t initial) {
    QueueHandle_t semaphore = __real_xQueueCreateCountingSemaphore(max, initial);
    if (send_queue_size != 0 && max == send_queue_size && initial == max) send_credits = semaphore;
    return semaphore;
}

esp_err_t __real_sdio_slave_initialize(sdio_slave_config_t* config);
esp_err_t __wrap_sdio_slave_initialize(sdio_slave_config_t* config) {
    // Above esp_hosted's tasks (21), so a round runs whole
    if (resetter == NULL && xTaskCreate(reset_sdio, "reset sdio", 3072, NULL, 22, &resetter) != pdPASS) return ESP_ERR_NO_MEM;
    esp_hosted_event = config->event_cb;
    config->event_cb = from_host;
    send_queue_size = config->send_queue_size;
    esp_err_t err = __real_sdio_slave_initialize(config);
    send_queue_size = 0;
    return err;
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

// Wakes the joining task
#define STARTED      BIT0
#define STOPPED      BIT1
#define CONNECTED    BIT2
#define DISCONNECTED BIT3
#define SCAN_DONE    BIT4
#define NEW_LIST     BIT5
#define HOLD         BIT6
#define ASKED        BIT7
// Two checks in a row did not reach the internet
#define UNREACHABLE BIT8
// Ends a scan or a join early
#define INTERRUPT (STOPPED | NEW_LIST | HOLD | ASKED)

#define JOIN_MS  15000
#define HOLD_MS  60000
#define SECOND   1000000LL
#define REACH_MS 3000
#define RECHECK  (30 * SECOND)
#define DEMOTED  (600 * SECOND)
// The prober's wake reasons
#define NEW_LEASE BIT0
#define ASKED_NOW BIT1

typedef enum { JOINED, FAILED, INTERRUPTED } join_result;
// A known network's strongest access point in a scan
typedef struct {
    int8_t rssi;
    uint8_t channel, bssid[6];
} sighting;

static StaticSemaphore_t send_lock_memory, list_lock_memory;
static StaticEventGroup_t radio_memory;
static SemaphoreHandle_t list_lock;
static EventGroupHandle_t radio;
// The known networks as the P4 last sent them (P4_WIFI_LIST), and kept in NVS
static uint8_t known[2 + C6_NETWORKS * 98];
static size_t known_size;
static volatile unsigned list_version, planned_version;
static volatile bool held, hold_answer_due;
static volatile int64_t held_until;
static volatile uint8_t last_reason;
// P4_WIFI_JOIN's body until the joining task takes it
static uint8_t asked[2 + 32 + 1 + 64];
static volatile bool asked_due;
// The last P4_INTERNET's
static volatile uint8_t internet_asked;
static TaskHandle_t prober;
static const char* const state_names[] = {"stopped", "scanning", "joining", "connected", "none in range", "join failed", "held"};
// What C6_WIFI_STATE last said, for P4_READY; until the task planned for a new list, its next report answers instead
static uint8_t state_message[5 + 32];
static size_t state_size;
static volatile bool state_due;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;

static void keep_list(const uint8_t* list, size_t size) {
    nvs_handle_t nvs;
    int count = networks(list, size);
    if (count < 0 || nvs_open("tab5", NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "known networks refused");
        return;
    }
    xSemaphoreTake(list_lock, portMAX_DELAY);
    bool same = size == known_size && memcmp(known, list, size) == 0;
    if (!same) {
        memset(known, 0, sizeof known);
        memcpy(known, list, known_size = size);
        list_version++;
    }
    xSemaphoreGive(list_lock);
    esp_err_t err = same ? ESP_OK : nvs_set_blob(nvs, "wifi", list, size);
    if (err == ESP_OK && !same) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (!same) xEventGroupSetBits(radio, NEW_LIST);
    ESP_LOGI(TAG, "%d known networks, Wi-Fi %s: %s", count, list[0] ? "on" : "off", same ? "unchanged" : esp_err_to_name(err));
}

// The P4's (P4_HOSTNAME), else CONFIG_LWIP_LOCAL_HOSTNAME
static char hostname[C6_HOSTNAME + 1];

static void use_hostname(void) {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (hostname[0] && netif != NULL) esp_netif_set_hostname(netif, hostname);
}

static void keep_hostname(const uint8_t* name, size_t size) {
    nvs_handle_t nvs;
    esp_err_t err = ESP_OK;
    if (size == 0 || size > C6_HOSTNAME) return;
    for (size_t i = 0; i < size; i++)
        if (!isalnum(name[i]) && name[i] != '-') return;
    bool same = strlen(hostname) == size && memcmp(hostname, name, size) == 0;
    if (!same) {
        memcpy(hostname, name, size);
        hostname[size] = 0;
        use_hostname();
        if ((err = nvs_open("tab5", NVS_READWRITE, &nvs)) == ESP_OK) {
            if ((err = nvs_set_str(nvs, "hostname", hostname)) == ESP_OK) err = nvs_commit(nvs);
            nvs_close(nvs);
        }
    }
    ESP_LOGI(TAG, "hostname %s: %s", hostname, same ? "unchanged" : esp_err_to_name(err));
}

static void load_list(void) {
    nvs_handle_t nvs;
    size_t size = sizeof known;
    if (nvs_open("tab5", NVS_READONLY, &nvs) != ESP_OK) return;
    if (nvs_get_blob(nvs, "wifi", known, &size) == ESP_OK && networks(known, size) >= 0) known_size = size;
    size = sizeof hostname;
    if (nvs_get_str(nvs, "hostname", hostname, &size) != ESP_OK) hostname[0] = 0;
    nvs_close(nvs);
}

// Network index of the known ones, at its SSID length; the caller holds list_lock
static const uint8_t* known_network(int index) {
    const uint8_t* at = known + 2;
    if (known_size < 2 || index >= known[1]) return NULL;
    for (int i = 0; i < index; i++)
        at += 1 + at[0], at += 1 + at[0];
    return at;
}

static int known_index(const char* ssid) {
    const uint8_t* n;
    int found = -1;
    xSemaphoreTake(list_lock, portMAX_DELAY);
    for (int i = 0; found < 0 && (n = known_network(i)) != NULL; i++)
        if (strlen(ssid) == n[0] && memcmp(ssid, n + 1, n[0]) == 0) found = i;
    xSemaphoreGive(list_lock);
    return found;
}

// An answer is sent even when nothing changed
static void report(uint8_t state, const char* ssid, int8_t rssi, bool answer) {
    uint8_t message[sizeof state_message] = {C6_WIFI_STATE, state, last_reason, (uint8_t)rssi, (uint8_t)strnlen(ssid, 32)};
    size_t size = 5 + message[4];
    memcpy(message + 5, ssid, message[4]);
    portENTER_CRITICAL(&state_lock);
    bool same = size == state_size && memcmp(message, state_message, size) == 0;
    memcpy(state_message, message, state_size = size);
    portEXIT_CRITICAL(&state_lock);
    if (!same) ESP_LOGI(TAG, "Wi-Fi %s %s, reason %u, %d dBm", state_names[state], ssid, last_reason, rssi);
    if ((!same || answer || state_due) && p4_reached) state_due = false, send_to_p4(message, size);
}

static EventBits_t await(EventBits_t bits, int64_t us) {
    return xEventGroupWaitBits(radio, bits, pdTRUE, pdFALSE, us < 0 ? portMAX_DELAY : pdMS_TO_TICKS(us / 1000) + 1);
}

static void on_wifi(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg, (void)base;
    if (id == WIFI_EVENT_STA_DISCONNECTED) last_reason = (uint8_t)((wifi_event_sta_disconnected_t*)data)->reason;
    EventBits_t bit = id == WIFI_EVENT_STA_START          ? STARTED
                      : id == WIFI_EVENT_STA_STOP         ? STOPPED
                      : id == WIFI_EVENT_STA_CONNECTED    ? CONNECTED
                      : id == WIFI_EVENT_STA_DISCONNECTED ? DISCONNECTED
                      : id == WIFI_EVENT_SCAN_DONE        ? SCAN_DONE
                                                          : 0;
    if (bit != 0) xEventGroupSetBits(radio, bit);
}

static void on_lease(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg, (void)base, (void)id, (void)data;
    xTaskNotify(prober, NEW_LEASE, eSetBits);
}

// Each known network's strongest access point, at INT8_MIN when out of reach; ESP_ERR_INVALID_STATE when a stop, a new list or a
// hold ended it
static esp_err_t scan(sighting seen[C6_NETWORKS]) {
    const uint8_t* n;
    wifi_ap_record_t ap;
    uint16_t count = 0;
    char line[120];
    int length = 0;
    for (int k = 0; k < C6_NETWORKS; k++)
        seen[k].rssi = INT8_MIN;
    xEventGroupClearBits(radio, SCAN_DONE);
    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err != ESP_OK) return err;
    EventBits_t bits = await(SCAN_DONE | INTERRUPT, 10 * SECOND);
    if (!(bits & SCAN_DONE)) {
        esp_wifi_scan_stop();
        await(SCAN_DONE, SECOND);
        esp_wifi_clear_ap_list();
        return (bits & INTERRUPT) ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
    }
    esp_wifi_scan_get_ap_num(&count);
    xSemaphoreTake(list_lock, portMAX_DELAY);
    for (uint16_t i = 0; i < count && esp_wifi_scan_get_ap_record(&ap) == ESP_OK; i++)
        for (int k = 0; (n = known_network(k)) != NULL; k++)
            if (strlen((const char*)ap.ssid) == n[0] && memcmp(ap.ssid, n + 1, n[0]) == 0 && ap.rssi > seen[k].rssi) {
                seen[k] = (sighting){.rssi = ap.rssi, .channel = ap.primary};
                memcpy(seen[k].bssid, ap.bssid, sizeof ap.bssid);
            }
    for (int k = 0; (n = known_network(k)) != NULL; k++)
        if (seen[k].rssi != INT8_MIN && length < (int)sizeof line)
            length += snprintf(line + length, sizeof line - length, " %.*s %d", n[0], n + 1, seen[k].rssi);
    xSemaphoreGive(list_lock);
    esp_wifi_clear_ap_list();
    ESP_LOGI(TAG, "scan: %u networks, known in reach:%s", count, length > 0 ? line : " none");
    return ESP_OK;
}

// Known network index, or network ([SSID length][SSID][password length][password]) when not NULL, at the access point a scan
// just saw, which spares the scan of every channel a join makes; the strongest when at is NULL
static join_result join(int index, const uint8_t* network, const sighting* at, char ssid[33]) {
    wifi_config_t cfg = {.sta = {.scan_method = at != NULL ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN,
                                 .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
                                 .pmf_cfg.capable = true,
                                 .bssid_set = at != NULL,
                                 .channel = at != NULL ? at->channel : 0}};
    if (at != NULL) memcpy(cfg.sta.bssid, at->bssid, sizeof cfg.sta.bssid);
    xSemaphoreTake(list_lock, portMAX_DELAY);
    const uint8_t* n = network != NULL ? network : known_network(index);
    if (n != NULL) {
        memcpy(cfg.sta.ssid, n + 1, n[0]);
        memcpy(cfg.sta.password, n + 2 + n[0], n[1 + n[0]]);
        cfg.sta.threshold.authmode = n[1 + n[0]] > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    }
    xSemaphoreGive(list_lock);
    if (n == NULL) return FAILED;
    memcpy(ssid, cfg.sta.ssid, 32);
    ssid[32] = 0;
    report(C6_WIFI_JOINING, ssid, at != NULL ? at->rssi : 0, false);
    // An UNREACHABLE from before is about the network left
    xEventGroupClearBits(radio, CONNECTED | DISCONNECTED | UNREACHABLE);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(&cfg, 0, sizeof cfg);
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "join %s: %s", ssid, esp_err_to_name(err));
        return FAILED;
    }
    EventBits_t bits = await(CONNECTED | DISCONNECTED | INTERRUPT, JOIN_MS * 1000LL);
    if (bits & CONNECTED) return JOINED;
    if (bits & DISCONNECTED) return FAILED;
    // The P4 sees this disconnect, so it never holds a lease of another network
    esp_wifi_disconnect();
    await(DISCONNECTED, SECOND);
    // For the disconnect to reach the P4 before the answer to its hold
    if (bits & HOLD) vTaskDelay(pdMS_TO_TICKS(100));
    return (bits & INTERRUPT) ? INTERRUPTED : FAILED;
}

// Reasons a wrong password gives back off longer
static int64_t backoff(uint8_t reason, uint8_t failures) {
    bool refused = reason == WIFI_REASON_AUTH_EXPIRE || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_AUTH_FAIL ||
                   reason == WIFI_REASON_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
                   reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD;
    int64_t wait = (refused ? 60 : 10) * SECOND << (failures < 5 ? failures : 5);
    return wait < 300 * SECOND ? wait : 300 * SECOND;
}

static int next_known(const sighting seen[C6_NETWORKS], const int64_t backoff_until[C6_NETWORKS], const int64_t demoted_until[C6_NETWORKS],
                      uint32_t skip) {
    int8_t rssi[C6_NETWORKS];
    for (int i = 0; i < C6_NETWORKS; i++)
        rssi[i] = seen[i].rssi;
    return c6_next_network(rssi, backoff_until, demoted_until, C6_NETWORKS, skip, esp_timer_get_time());
}

// P4_WIFI_JOIN: this network now, the one joined before left only once Wi-Fi is up for it
static void join_asked(char ssid[33]) {
    static const char* const results[] = {"joined", "not found", "failed", "Wi-Fi stopped", "interrupted"};
    uint8_t network[sizeof asked];
    wifi_ap_record_t ap = {0};
    xSemaphoreTake(list_lock, portMAX_DELAY);
    memcpy(network, asked, sizeof network);
    memset(asked, 0, sizeof asked);
    asked_due = false;
    xSemaphoreGive(list_lock);
    uint8_t result = C6_JOIN_STOPPED;
    held = false;
    // The P4 starts Wi-Fi right before it asks
    if (!eh_cp_feat_wifi_is_started()) await(STARTED, 2 * SECOND);
    if (eh_cp_feat_wifi_is_started() && eh_cp_feat_wifi_is_station_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
        strlen((const char*)ap.ssid) == network[1] && memcmp(ap.ssid, network + 2, network[1]) == 0) {
        result = C6_JOIN_DONE;
        strlcpy(ssid, (const char*)ap.ssid, 33);
    } else if (eh_cp_feat_wifi_is_started()) {
        if (eh_cp_feat_wifi_is_station_connected()) {
            esp_wifi_disconnect();
            await(DISCONNECTED, SECOND);
        }
        last_reason = 0;
        join_result r = join(-1, network + 1, NULL, ssid);
        result = r == JOINED                              ? C6_JOIN_DONE
                 : r == INTERRUPTED                       ? C6_JOIN_INTERRUPTED
                 : last_reason == WIFI_REASON_NO_AP_FOUND ? C6_JOIN_NOT_FOUND
                                                          : C6_JOIN_FAILED;
    }
    memset(network + 1, 0, sizeof network - 1);
    ESP_LOGI(TAG, "join asked by the P4: %s, reason %u", results[result], last_reason);
    send_to_p4((const uint8_t[]){C6_WIFI_JOINED, network[0], result, last_reason}, 4);
}

// After two checks that did not reach the internet: another known network in reach, this one tried after the others for a while
static void leave_unreachable(const char* ssid, sighting seen[C6_NETWORKS], const int64_t backoff_until[C6_NETWORKS],
                              int64_t demoted_until[C6_NETWORKS]) {
    int current = known_index(ssid), next = -1;
    if (current >= 0) demoted_until[current] = esp_timer_get_time() + DEMOTED;
    // Reported as joined still: the P4 keeps the link meanwhile
    if (scan(seen) == ESP_OK) next = next_known(seen, backoff_until, demoted_until, current >= 0 ? 1u << current : 0);
    if (next < 0 || esp_timer_get_time() < demoted_until[next]) {
        ESP_LOGW(TAG, "%s does not reach the internet, and no other known network is in reach", ssid);
        return;
    }
    ESP_LOGW(TAG, "%s does not reach the internet: leaving it for another known network", ssid);
    esp_wifi_disconnect();
    await(DISCONNECTED, SECOND);
}

// The first known network in list order a scan sees above C6_RSSI_FLOOR, the next when it fails; none rescans after 5 s doubling
// to 60 s. No scans while joined (they would stall SSH), and a drop retries the network once first. Joins whenever Wi-Fi was
// started, by itself or by the P4's wifi on, while the list says on
static void join_known(void* arg) {
    static const int rescans[] = {5, 10, 20, 40, 60};
    int64_t backoff_until[C6_NETWORKS] = {0}, demoted_until[C6_NETWORKS] = {0};
    uint8_t failures[C6_NETWORKS] = {0};
    unsigned version = planned_version = list_version;
    bool was_on = known_size >= 2 && known[0];
    int rescan = 0;
    char ssid[33] = "";
    bool retry = false;
    (void)arg;
    // esp_hosted's event handlers, which forward the joins to the P4, register in its own init task
    while (g_auto_feat_init_done_eg == NULL)
        vTaskDelay(pdMS_TO_TICKS(10));
    xEventGroupWaitBits(g_auto_feat_init_done_eg, EH_CP_FEAT_INIT_DONE_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    // Network Split's init raises them to debug
    esp_log_level_set("esp_netif_lwip", ESP_LOG_INFO);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_INFO);
    // Before the first lease
    use_hostname();
    // The P4's esp_wifi_init then finds the same parameters (CONFIG_WIFI_RMT_* equal to these)
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    // The P4's own over RPC may be under way, which refuses this one until it is done
    esp_err_t err;
    for (int tries = 0; (err = esp_wifi_init(&init)) != ESP_OK && tries < 20; tries++)
        vTaskDelay(pdMS_TO_TICKS(100));
    // The default flash storage would keep the passwords in nvs.net80211 too
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    if (err == ESP_OK) err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_lease, NULL);
    if (err == ESP_OK && known_size >= 2 && known[0]) err = esp_wifi_start();
    ESP_LOGI(TAG, "Wi-Fi %s, heap %u free", esp_err_to_name(err), (unsigned)esp_get_free_heap_size());
    if (err != ESP_OK) vTaskDelete(NULL);
    for (;;) {
        bool on = known_size >= 2 && known[0], started = eh_cp_feat_wifi_is_started(), connected = eh_cp_feat_wifi_is_station_connected();
        wifi_ap_record_t ap = {0};
        sighting seen[C6_NETWORKS];
        if (connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) strlcpy(ssid, (const char*)ap.ssid, sizeof ssid);
        if (version != list_version) {
            version = planned_version = list_version;
            memset(backoff_until, 0, sizeof backoff_until);
            memset(demoted_until, 0, sizeof demoted_until);
            memset(failures, 0, sizeof failures);
            rescan = 0;
            // Only a change of the saved state stops or starts Wi-Fi: the P4 may start it for its own join before the list says
            // on, and its wifi off without save lasts until its restart
            if (on != was_on && on != started) {
                was_on = on;
                on ? esp_wifi_start() : esp_wifi_stop();
                await(on ? STARTED : STOPPED, SECOND);
                continue;
            }
            was_on = on;
        }
        if (asked_due) {
            join_asked(ssid);
            rescan = 0, retry = false;
            continue;
        }
        if (held && esp_timer_get_time() >= held_until) held = false;
        if (held) {
            report(connected ? C6_WIFI_CONNECTED : C6_WIFI_HELD, connected ? ssid : "", ap.rssi, hold_answer_due);
            hold_answer_due = false;
            int64_t left = held_until - esp_timer_get_time();
            await(STARTED | STOPPED | NEW_LIST | HOLD | ASKED | CONNECTED | DISCONNECTED, left > 0 ? left : 0);
            rescan = 0, retry = false;
            continue;
        }
        if (!started || !on) {
            report(C6_WIFI_STOPPED, "", 0, false);
            await(STARTED | NEW_LIST | HOLD | ASKED, -1);
            rescan = 0, retry = false;
            continue;
        }
        if (connected) {
            report(C6_WIFI_CONNECTED, ssid, ap.rssi, false);
            EventBits_t bits = await(DISCONNECTED | INTERRUPT | UNREACHABLE, -1);
            if ((bits & UNREACHABLE) && eh_cp_feat_wifi_is_station_connected()) leave_unreachable(ssid, seen, backoff_until, demoted_until);
            // An old disconnect, from before this join, is no drop
            retry = (bits & DISCONNECTED) && !eh_cp_feat_wifi_is_station_connected();
            rescan = 0;
            continue;
        }
        join_result result = FAILED;
        int index = retry ? known_index(ssid) : -1;
        retry = false;
        if (index >= 0 && (result = join(index, NULL, NULL, ssid)) != FAILED) continue;
        report(C6_WIFI_SCANNING, "", 0, false);
        err = scan(seen);
        bool tried = false;
        uint32_t skip = 0;
        for (int i; err == ESP_OK && result == FAILED && (i = next_known(seen, backoff_until, demoted_until, skip)) >= 0;) {
            skip |= 1u << i;
            tried = true;
            result = join(i, NULL, &seen[i], ssid);
            if (result == JOINED) failures[i] = 0;
            if (result == FAILED) backoff_until[i] = esp_timer_get_time() + backoff(last_reason, failures[i]++);
        }
        if (result != FAILED || err == ESP_ERR_INVALID_STATE) continue;
        if (err != ESP_OK)
            ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
        else
            report(tried ? C6_WIFI_JOIN_FAILED : C6_WIFI_NONE_IN_RANGE, "", 0, false);
        await(INTERRUPT | STARTED, rescans[rescan] * SECOND);
        if (rescan < 4) rescan++;
    }
}

// A TCP connection to address port 443: 0 or an errno
static int reach(const char* address) {
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(443), .sin_addr.s_addr = inet_addr(address)};
    int rc = 0, fd;
    socklen_t length = sizeof rc;
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) != 0)
        rc = errno;
    else if (connect(fd, (struct sockaddr*)&to, sizeof to) != 0 && (rc = errno) == EINPROGRESS) {
        if (poll(&(struct pollfd){.fd = fd, .events = POLLOUT}, 1, REACH_MS) != 1)
            rc = ETIMEDOUT;
        else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &rc, &length) != 0)
            rc = errno;
    }
    if (fd >= 0) close(fd);
    return rc;
}

// The internet at each lease, when the P4 asks, and every RECHECK while it fails; two failures in a row on one network tell
// the joining task
static void probe(void* arg) {
    static const char* const addresses[] = C6_INTERNET_ADDRESSES;
    const size_t count = sizeof addresses / sizeof *addresses;
    uint8_t failures = 0;
    bool failing = false;
    int64_t probed_at = 0;
    (void)arg;
    for (;;) {
        uint32_t why = 0;
        int64_t left = probed_at + RECHECK - esp_timer_get_time();
        xTaskNotifyWait(0, UINT32_MAX, &why, !failing ? portMAX_DELAY : left > 0 ? pdMS_TO_TICKS(left / 1000) + 1 : 0);
        if (why & NEW_LEASE) failures = 0;
        uint8_t sequence = (why & ASKED_NOW) ? internet_asked : 0;
        esp_netif_ip_info_t ip = {0};
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        bool up = eh_cp_feat_wifi_is_station_connected() && netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
        if (!up) failing = false;
        if ((!up && sequence == 0) || (why == 0 && !failing)) continue;
        int64_t start = esp_timer_get_time();
        int err = ENETUNREACH;
        size_t i = 0;
        while (up && i < count && (err = reach(addresses[i])) != 0)
            i++;
        const char* address = addresses[MIN(i, count - 1)];
        unsigned ms = (unsigned)((esp_timer_get_time() - start) / 1000);
        probed_at = esp_timer_get_time();
        if (p4_reached) {
            uint32_t to = inet_addr(address);
            uint8_t message[] = {C6_INTERNET, sequence,           (uint8_t)err,        (uint8_t)MIN(ms, 0xffffu), (uint8_t)(MIN(ms, 0xffffu) >> 8),
                                 (uint8_t)to, (uint8_t)(to >> 8), (uint8_t)(to >> 16), (uint8_t)(to >> 24)};
            send_to_p4(message, sizeof message);
        }
        ESP_LOGI(TAG, "internet %s: %s after %u ms%s", up ? address : "-", err == 0 ? "reached" : strerror(err), ms, sequence != 0 ? ", asked" : "");
        if (!up) continue;
        failing = err != 0;
        failures = failing ? failures + 1 : 0;
        if (failures >= 2) {
            failures = 0;
            xEventGroupSetBits(radio, UNREACHABLE);
        }
    }
}

// The answer's own buffer: esp_hosted runs this on a 4 KiB stack
static void nvs_request(uint8_t sequence, uint8_t operation, const uint8_t* body, size_t size) {
    static uint8_t answer[6 + C6_NVS_MESSAGE];
    esp_err_t err;
    size_t length = c6_nvs_serve(operation, body, size, answer + 6, &err);
    answer[0] = C6_NVS;
    answer[1] = sequence;
    for (int i = 0; i < 4; i++)
        answer[2 + i] = (uint8_t)((uint32_t)err >> 8 * i);
    send_to_p4(answer, 6 + length);
    memset(answer, 0, sizeof answer);
}

static void from_p4(uint32_t id, const uint8_t* data, size_t size, void* context) {
    (void)id, (void)context;
    if (size >= 3 && data[0] == P4_NVS) nvs_request(data[1], data[2], data + 3, size - 3);
    if (size >= 1 && data[0] == P4_HOSTNAME) keep_hostname(data + 1, size - 1);
    if (size >= 1 && data[0] == P4_WIFI_LIST) keep_list(data + 1, size - 1);
    if (size >= 4 && data[0] == P4_WIFI_JOIN && data[2] >= 1 && data[2] <= 32 && size > 3u + data[2] && data[3 + data[2]] <= 64 &&
        size == 4u + data[2] + data[3 + data[2]]) {
        xSemaphoreTake(list_lock, portMAX_DELAY);
        memcpy(asked, data + 1, size - 1);
        asked_due = true;
        xSemaphoreGive(list_lock);
        xEventGroupSetBits(radio, ASKED);
    }
    if (size >= 2 && data[0] == P4_INTERNET) {
        internet_asked = data[1];
        xTaskNotify(prober, ASKED_NOW, eSetBits);
    }
    if (size >= 2 && data[0] == P4_WIFI_HOLD) {
        held_until = esp_timer_get_time() + HOLD_MS * 1000LL;
        held = hold_answer_due = data[1] != 0;
        xEventGroupSetBits(radio, HOLD);
    }
    if (size >= 1 && data[0] == P4_READY) {
        uint8_t message[sizeof state_message];
        size_t length;
        held = false;
        xEventGroupSetBits(radio, HOLD);
        eh_cp_feat_wifi_replay_connected_event_if_needed();
        eh_cp_feat_nw_split_replay_status_if_needed();
        portENTER_CRITICAL(&state_lock);
        memcpy(message, state_message, length = state_size);
        portEXIT_CRITICAL(&state_lock);
        if (length > 0 && planned_version == list_version)
            send_to_p4(message, length);
        else
            state_due = true;
    }
    if (size < 1 || data[0] != P4_KEEP) return;
    if (unchecked() && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) ESP_LOGI(TAG, "image kept");
    send_to_p4((const uint8_t[]){C6_HELLO, C6_PROTOCOL}, 2);
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
    send_lock = xSemaphoreCreateMutexStatic(&send_lock_memory);
    list_lock = xSemaphoreCreateMutexStatic(&list_lock_memory);
    radio = xEventGroupCreateStatic(&radio_memory);
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
    load_list();
    if (xTaskCreate(probe, "probe", 4096, NULL, 4, &prober) != pdPASS) ESP_LOGE(TAG, "no prober");
    ESP_ERROR_CHECK(eh_cp_feat_peer_data_init());
    ESP_ERROR_CHECK(eh_cp_feat_peer_data_register_callback(P4_TO_C6, from_p4, NULL));
    // Last, so nothing it does can keep a new image from being kept
    if (xTaskCreate(join_known, "join known", 4096, NULL, 5, NULL) != pdPASS) ESP_LOGE(TAG, "no joining task");
}
