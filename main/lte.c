// SPDX-License-Identifier: GPL-3.0-or-later
#include "lte.h"

#include <ctype.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_attr.h>
#include <esp_check.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_ppp.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <lwip/inet.h>

#include "probe.h"
#include "sleep.h"

#define UART     UART_NUM_2
#define TX       6
#define RX       46
#define LINE     128
#define SECOND   NETWORK_SECOND
#define GOT_IP   BIT0
#define DEAD     BIT1
#define CHECK_MS 10000
// Per server: the path's two, then a public one
#define DNS_MS 4000

static const char* TAG = "lte";
// An LCP Terminate-Request with every control byte escaped: a modem left in PPP by an earlier boot answers NO CARRIER,
// one in command mode ignores it
static const uint8_t terminate[] = {0x7e, 0xff, 0x7d, 0x23, 0xc0, 0x21, 0x7d, 0x25, 0x7d, 0x21, 0x7d, 0x20, 0x7d, 0x24, 0x3d, 0xc7, 0x7e};
static void (*changed)(void);
static TaskHandle_t task;
static QueueHandle_t lines, commands;
static EventGroupHandle_t events;
static esp_netif_t* netif;
static atomic_bool dialing, data_mode, on, turned_on, suspected, dead_probe;
// When the modem last left a command unanswered: asking a quiet modem again keeps it quiet longer
static int64_t unanswered = -90 * SECOND;
static atomic_bool answered; // this boot
// The modem spoke unasked, e.g. NO CARRIER once a PPP link of an earlier boot timed out: it can be asked again at once
static atomic_bool heard;
// Read by the task, copied out under guard
EXT_RAM_BSS_ATTR static modem_snapshot fresh, shown;
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static bool kept; // this boot
static lte_view view;
static portMUX_TYPE view_lock = portMUX_INITIALIZER_UNLOCKED;
static _Atomic int64_t received_at;

static void tell(void) {
    if (changed != NULL) changed();
}

static void set_state(lte_state s) {
    taskENTER_CRITICAL(&view_lock);
    bool changes = view.state != s;
    view.state = s;
    taskEXIT_CRITICAL(&view_lock);
    if (changes) tell();
}

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
            if (strcmp(line, "OK") == 0 || strcmp(line, "NO CARRIER") == 0 || strcmp(line, "RDY") == 0 || line[0] == '+' || line[0] == '$') {
                atomic_store(&heard, true), atomic_store(&answered, true);
                if (task != NULL) xTaskNotifyGive(task);
            }
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
        if (reply != NULL && modem_kept(line, prefix, reply)) strlcpy(reply, line, LINE);
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
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCoreWithCaps(reader, "lte reader", 4096, NULL, 6, NULL, 1, MALLOC_CAP_SPIRAM) == pdPASS, ESP_ERR_NO_MEM,
                        TAG, "reader");
    open = true;
    return ESP_OK;
}

// Answering in command mode
static esp_err_t answering(void) {
    char line[LINE];
    if (data_mode) return ESP_ERR_INVALID_STATE;
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

static bool up(void) {
    return events != NULL && (xEventGroupGetBits(events) & (GOT_IP | DEAD)) == GOT_IP;
}

void lte_log_snapshot(void) {
    modem_snapshot* s = heap_caps_malloc(sizeof *s, MALLOC_CAP_SPIRAM);
    if (s == NULL) return;
    lte_snapshot(s);
    int64_t now = esp_timer_get_time();
    if (up()) ESP_LOGI(TAG, "modem: PPP is up, values from before the dial (lte off to read again)");
    char logged[sizeof s->values[0]];
    for (int i = 0; i < MODEM_ITEMS; i++) {
        if (s->read_at[i] == 0) continue;
        modem_mask(s->values[i], logged, sizeof logged);
        ESP_LOGI(TAG, "modem %s: %s (%lld s ago)", modem_commands[i].name, logged, (now - s->read_at[i]) / 1000000);
    }
    free(s);
}

// Queued by lte_command; modem at sends anything (the owner's), every reply line logged by the reader
static void run_command(const char* line) {
    esp_netif_ip_info_t ip;
    sleep_hold(true);
    if (strcmp(line, "lte status") == 0 && data_mode && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        ESP_LOGI(TAG, "lte status: PPP, ip " IPSTR, IP2STR(&ip.ip));
    } else if (strcmp(line, "lte status") == 0 || strcmp(line, "modem read") == 0) {
        esp_err_t err = data_mode ? ESP_ERR_INVALID_STATE : ready(line[0] == 'm');
        ESP_LOGI(TAG, "%s: %s", line, esp_err_to_name(err));
        if (line[0] == 'm') lte_log_snapshot();
    } else if (strncmp(line, "modem at ", 9) == 0) {
        if (data_mode)
            ESP_LOGW(TAG, "modem at: PPP is up, lte off first");
        else if (open_uart() == ESP_OK)
            ESP_LOGI(TAG, "%s: %s", line, at(line + 9, "", NULL) ? "OK" : "no OK");
    } else if (strcmp(line, "lte probe dead on") == 0 || strcmp(line, "lte probe dead off") == 0) {
        dead_probe = line[16] == 'n';
        ESP_LOGI(TAG, "LTE checks %s", dead_probe ? "192.0.2.1, which never answers" : "the public addresses");
    }
    sleep_hold(false);
}

static void run_commands(void) {
    char line[80];
    while (xQueueReceive(commands, line, 0) == pdTRUE)
        run_command(line);
}

// 0 for an answer; EINTR when it does not count: a sleep inside, PPP gone meanwhile, no socket free
static int check_link(char address[16]) {
    lte_view before, after;
    uint32_t found;
    lte_view_now(&before);
    int rc;
    if (dead_probe) {
        strlcpy(address, "192.0.2.1", 16);
        rc = probe_connect(netif, inet_addr(address), 443, CHECK_MS);
    } else if ((rc = probe_internet(netif, CHECK_MS, address)) != 0 && rc != EINTR) {
        // The carrier's DNS answering proves the bearer where the public addresses are blocked
        probe_lookup l = probe_resolve(netif, "dns.google", DNS_MS, &found);
        if (l == PROBE_FOUND || l == PROBE_NO_NAME) rc = 0, strlcpy(address, "its DNS", 16);
        if (l == PROBE_SLEPT) rc = EINTR;
    }
    lte_view_now(&after);
    if (rc == ECONNREFUSED) rc = 0;
    if (rc == EMFILE || rc == ENFILE || !up() || after.epoch != before.epoch) rc = EINTR;
    return rc;
}

static void set_internet(network_health health, int64_t now) {
    taskENTER_CRITICAL(&view_lock);
    view.internet = health, view.internet_at = now;
    taskEXIT_CRITICAL(&view_lock);
    tell();
}

static void log_wait(int64_t next_dial, int64_t now) {
    if (next_dial == NETWORK_NEVER)
        ESP_LOGW(TAG, "no modem: LTE waits for lte on or for the modem to speak");
    else
        ESP_LOGI(TAG, "LTE again in %lld s", (next_dial - now) / SECOND);
}

// The research's rules: checks read-only before each dial, no dial within 60 s of the last, a lost link waits 120 s doubling to
// 960 s. LTE never gives up (owner, 10-15); only a modem that never answered this boot waits until it speaks, as no Stamp may sit
// on G6 (one left in PPP by an earlier boot says NO CARRIER once that link times out). Network events never end a wait
static void run(void* arg) {
    (void)arg;
    int drops = 0, deaths = 0;
    // The last link a check found good: only one proven can have had its bearer remade
    unsigned proven = 0;
    int64_t next_dial = 0, last_dial = -60 * SECOND, last_death = -120 * 60 * SECOND, first_failed = 0, checked_at = 0;
    for (;;) {
        run_commands();
        int64_t now = esp_timer_get_time(), wait = NETWORK_NEVER;
        lte_view v;
        lte_view_now(&v);
        if (atomic_exchange(&turned_on, false) && on) next_dial = lte_dial_at(LTE_ASKED, 0, &drops, &deaths, last_death, last_dial, now);
        if (next_dial == NETWORK_NEVER && atomic_load(&heard)) {
            ESP_LOGI(TAG, "the modem spoke: LTE again");
            next_dial = now;
        }
        if (!on) {
            disconnect();
            set_state(LTE_OFF);
        } else if (up() && v.state != LTE_UP) {
            // lte off and on again before this task saw the off: the link stayed
            set_state(LTE_UP);
        } else if (v.state == LTE_UP && !up()) {
            next_dial = lte_dial_at(LTE_UP_ENDED, now - v.up_at, &drops, &deaths, last_death, last_dial, now);
            set_state(LTE_WAITING);
            log_wait(next_dial, now);
            wait = next_dial;
        } else if (!up()) {
            if (now >= next_dial) {
                set_state(LTE_DIALING);
                sleep_hold(true);
                esp_err_t err = dial();
                sleep_hold(false);
                last_dial = now = esp_timer_get_time();
                first_failed = 0;
                if (err != ESP_OK) {
                    lte_outcome outcome = err == ESP_ERR_NOT_FOUND ? (answered ? LTE_SILENT : LTE_NEVER_ANSWERED)
                                          : err == ESP_FAIL        ? LTE_REFUSED
                                                                   : LTE_NOT_READY;
                    next_dial = lte_dial_at(outcome, 0, &drops, &deaths, last_death, last_dial, now);
                    set_state(outcome == LTE_NEVER_ANSWERED ? LTE_NO_MODEM : outcome == LTE_NOT_READY ? LTE_REGISTERING : LTE_WAITING);
                    log_wait(next_dial, now);
                }
                continue;
            }
            wait = next_dial;
        } else {
            int64_t check_at = lte_check_at(&v, first_failed, checked_at, suspected, now);
            if (check_at <= now) {
                char address[16] = "";
                suspected = false;
                int rc = check_link(address);
                now = esp_timer_get_time();
                // What sessions received during the check counts too
                lte_view_now(&v);
                if (rc != EINTR) checked_at = now;
                if (rc == 0) {
                    first_failed = 0;
                    proven = v.epoch;
                    ESP_LOGI(TAG, "internet %s reached", address);
                    set_internet(NETWORK_GOOD, now);
                } else if (rc != EINTR && lte_bearer_dead(v.up_at, first_failed, now, v.received_at)) {
                    esp_netif_ip_info_t ip = {0};
                    char ppp[16];
                    esp_netif_get_ip_info(netif, &ip);
                    esp_ip4addr_ntoa(&ip.ip, ppp, sizeof ppp);
                    ESP_LOGW(TAG, "internet %s: %s, as %lld s ago: the bearer is dead, PPP %s ended", address, strerror(rc),
                             (now - first_failed) / SECOND, ppp);
                    set_internet(NETWORK_BAD, now);
                    disconnect();
                    bool remade = false;
                    if (!data_mode && ask(MODEM_ADDRESS) >= 0) {
                        remade = proven == v.epoch && modem_address_moved(fresh.values[MODEM_ADDRESS], ppp);
                        ESP_LOGW(TAG, "the modem's address now: %s%s", fresh.values[MODEM_ADDRESS], remade ? ", a new bearer" : "");
                    }
                    next_dial = lte_dial_at(remade ? LTE_BEARER_REMADE : LTE_BEARER_DEAD, now - v.up_at, &drops, &deaths, last_death, last_dial, now);
                    if (!remade) last_death = now;
                    first_failed = 0;
                    set_state(LTE_WAITING);
                    log_wait(next_dial, now);
                } else if (rc != EINTR) {
                    // Checking: no verdict yet, but a new result for whoever waits for one
                    set_internet(NETWORK_UNKNOWN, now);
                    // The first failure counts from 60 s into the link, and again once bytes came since
                    if (first_failed == 0 || first_failed - v.up_at < 60 * SECOND || v.received_at >= first_failed) first_failed = now;
                    ESP_LOGW(TAG, "internet %s: %s", address, strerror(rc));
                } else {
                    // No socket free stays so for a while: not again at once
                    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
                }
                continue;
            }
            wait = check_at;
        }
        int64_t left = wait == NETWORK_NEVER ? SECOND : wait - esp_timer_get_time();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(left <= 0 ? 0 : left < SECOND ? left / 1000 + 1 : 1000));
    }
}

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    (void)id;
    if (base == IP_EVENT) {
        ESP_LOGI(TAG, "ip " IPSTR, IP2STR(&((ip_event_got_ip_t*)data)->ip_info.ip));
        xEventGroupSetBits(events, GOT_IP);
        taskENTER_CRITICAL(&view_lock);
        view.state = LTE_UP, view.internet = NETWORK_UNKNOWN, view.epoch++, view.up_at = esp_timer_get_time();
        taskEXIT_CRITICAL(&view_lock);
    } else {
        // The modem's NO CARRIER and everything after it are lines again
        data_mode = false;
        xEventGroupSetBits(events, DEAD);
    }
    tell();
    if (task != NULL) xTaskNotifyGive(task);
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

esp_err_t lte_start(const global_state* global, void (*listener)(void)) {
    static esp_netif_driver_base_t driver = {.post_attach = attach};
    changed = listener;
    on = global->lte_enabled;
    lines = xQueueCreate(16, LINE);
    commands = xQueueCreate(4, 80);
    events = xEventGroupCreate();
    netif = esp_netif_new(&(esp_netif_config_t)ESP_NETIF_DEFAULT_PPP());
    ESP_RETURN_ON_FALSE(lines != NULL && commands != NULL && events != NULL && netif != NULL, ESP_ERR_NO_MEM, TAG, "objects");
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, &driver), TAG, "attach");
    // The modem asks for CHAP with an empty name
    ESP_RETURN_ON_ERROR(esp_netif_ppp_set_auth(netif, NETIF_PPP_AUTHTYPE_CHAP, "", ""), TAG, "auth");
    ESP_RETURN_ON_ERROR(esp_netif_ppp_set_params(netif, &(esp_netif_ppp_config_t){.ppp_phase_event_enabled = true}), TAG, "params");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, on_event, NULL), TAG, "events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(NETIF_PPP_STATUS, NETIF_PPP_PHASE_DEAD, on_event, NULL), TAG, "events");
    view.state = on ? LTE_DIALING : LTE_OFF;
    // Stack in PSRAM: a task there must never write flash
    return xTaskCreatePinnedToCoreWithCaps(run, "lte", 6144, NULL, 5, &task, 1, MALLOC_CAP_SPIRAM) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void lte_view_now(lte_view* out) {
    taskENTER_CRITICAL(&view_lock);
    *out = view;
    taskEXIT_CRITICAL(&view_lock);
    out->received_at = received_at;
}

esp_netif_t* lte_netif(void) {
    return netif;
}

void lte_turn(bool turn_on) {
    on = turn_on;
    turned_on = turn_on;
    lte_view v;
    lte_view_now(&v);
    // Its next dial waits 60 s after the last
    if (turn_on && v.state == LTE_OFF) set_state(LTE_WAITING);
    if (!turn_on) set_state(LTE_OFF);
    if (task != NULL) xTaskNotifyGive(task);
}

int64_t lte_suspect(void) {
    lte_view v;
    lte_view_now(&v);
    int64_t now = esp_timer_get_time();
    if (lte_check_at(&v, 0, v.up_at, true, now) > now) return 0;
    suspected = true;
    if (task != NULL) xTaskNotifyGive(task);
    // Three addresses and three DNS servers
    return now + (3 * CHECK_MS + 3 * DNS_MS + 1000) * 1000LL;
}

void lte_received(void) {
    received_at = esp_timer_get_time();
}

void lte_snapshot(modem_snapshot* out) {
    taskENTER_CRITICAL(&guard);
    *out = shown;
    taskEXIT_CRITICAL(&guard);
}

bool lte_command(const char* line) {
    char copy[80];
    if (strcmp(line, "lte status") != 0 && strcmp(line, "modem read") != 0 && strncmp(line, "modem at ", 9) != 0 &&
        strcmp(line, "lte probe dead on") != 0 && strcmp(line, "lte probe dead off") != 0)
        return false;
    strlcpy(copy, line, sizeof copy);
    if (commands == NULL || xQueueSend(commands, copy, 0) != pdTRUE) ESP_LOGW(TAG, "%s: busy", line);
    if (task != NULL) xTaskNotifyGive(task);
    return true;
}
