// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_values.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <esp_timer.h>

#include "clock.h"
#include "net.h"
#include "power.h"
#include "screen.h"

#define SECOND 1000000LL

static const global_state* state;
static net_info net;
static int millivolts = -1;
// One value at a time: the UI copies each before asking for the next
static char text[96];

static const char* const lte_states[] = {
    [NET_LTE_OFF] = "꺼짐",
    [NET_LTE_NO_MODEM] = "모뎀 없음",
    [NET_LTE_DIALING] = "다이얼 중",
    [NET_LTE_REGISTERING] = "등록 중",
    [NET_LTE_WAITING] = "대기, 다시 시도 예정",
    [NET_LTE_UP] = "연결됨",
};

// The network every 2 s (it asks the C6), the battery every 5 s
static void refresh(void) {
    static int64_t described = -2 * SECOND, measured = -5 * SECOND;
    int64_t now = esp_timer_get_time();
    if (now - described >= 2 * SECOND) net_describe(&net), described = now;
    if (now - measured >= 5 * SECOND) {
        if (!power_battery(&millivolts)) millivolts = -1;
        measured = now;
    }
}

// A 2S Li-ion pack under a light load, by cell voltage
static int percent(int mv) {
    static const int cell[] = {3300, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200}, level[] = {0, 5, 10, 30, 55, 70, 82, 92, 100};
    int v = mv / 2;
    if (v <= cell[0]) return 0;
    for (size_t i = 1; i < sizeof cell / sizeof cell[0]; i++)
        if (v < cell[i]) return level[i - 1] + (level[i] - level[i - 1]) * (v - cell[i - 1]) / (cell[i] - cell[i - 1]);
    return 100;
}

static const char* network(void) {
    if (strcmp(net.path, "Wi-Fi") == 0)
        snprintf(text, sizeof text, "Wi-Fi %s", net.ssid);
    else if (strcmp(net.path, "LTE") == 0)
        snprintf(text, sizeof text, "LTE");
    else
        snprintf(text, sizeof text, "망 없음 · LTE %s", net.lte_on ? lte_states[net.lte] : "꺼짐");
    return text;
}

static const char* value(void* user, const char* name) {
    (void)user;
    refresh();
    if (strcmp(name, "host") == 0) return state->current_host != NULL ? state->current_host->alias : "호스트 없음";
    if (strcmp(name, "status") == 0) return screen_status(text, sizeof text) ? text : NULL;
    if (strcmp(name, "network") == 0) return network();
    if (strcmp(name, "battery") == 0) {
        if (millivolts < 0) return NULL;
        if (state->battery == 1)
            snprintf(text, sizeof text, "%d%%", percent(millivolts));
        else
            snprintf(text, sizeof text, "%d.%02d V", millivolts / 1000, millivolts % 1000 / 10);
        return text;
    }
    if (strcmp(name, "time") == 0) {
        time_t now = time(NULL);
        struct tm local;
        if (!clock_known()) return "--:--";
        strftime(text, sizeof text, "%H:%M", localtime_r(&now, &local));
        return text;
    }
    if (strcmp(name, "path") == 0) {
        bool lte = strcmp(net.path, "LTE") == 0;
        if (strcmp(net.path, "none") == 0) return "없음";
        snprintf(text, sizeof text, "%s (%s)", net.path, lte ? net.lte_address : net.wifi_address);
        return text;
    }
    if (strcmp(name, "wifi.state") == 0) {
        if (!net.wifi_on) return state->wifi_off ? "꺼짐 (재시작 후에도)" : "꺼짐";
        if (!net.wifi_joined) return "찾는 중";
        snprintf(text, sizeof text, "%s %s", net.ssid, net.wifi_reaches ? "연결됨" : "연결됨, 호스트 확인 중");
        return text;
    }
    if (strcmp(name, "wifi.detail") == 0) {
        if (!net.wifi_joined) return NULL;
        snprintf(text, sizeof text, "%s · %d dBm", net.wifi_address, net.rssi);
        return text;
    }
    if (strcmp(name, "lte.state") == 0) {
        if (!net.lte_on) return state->lte_off ? "꺼짐 (재시작 후에도)" : "꺼짐";
        return lte_states[net.lte];
    }
    if (strcmp(name, "lte.detail") == 0) return net.lte_address[0] != 0 ? net.lte_address : NULL;
    if (strcmp(name, "countdown") == 0) {
        int64_t left = state->host_deadline_us - esp_timer_get_time();
        if (state->host_deadline_us == 0 || left < 0) return NULL;
        snprintf(text, sizeof text, "%d초 뒤 접속 · 누르거나 키를 치면 기다림", (int)(left / SECOND) + 1);
        return text;
    }
    return NULL;
}

static const ssh_host* host(size_t index) {
    const ssh_host* h = state->hosts;
    for (; h != NULL && index > 0; index--)
        h = h->next;
    return h;
}

// Forgotten networks keep their place with an empty SSID
static const wifi_network* network_at(size_t index) {
    for (const wifi_network* w = state->wifi; w != NULL; w = w->next)
        if (w->ssid[0] != 0 && index-- == 0) return w;
    return NULL;
}

static size_t count(void* user, const char* name) {
    (void)user;
    size_t n = 0;
    if (strcmp(name, "hosts") == 0)
        while (host(n) != NULL)
            n++;
    if (strcmp(name, "wifi") == 0)
        while (network_at(n) != NULL)
            n++;
    return n;
}

static const char* item(void* user, const char* name, size_t index, const char* field) {
    (void)user;
    if (strcmp(name, "wifi") == 0) {
        const wifi_network* w = network_at(index);
        return w != NULL && strcmp(field, "ssid") == 0 ? w->ssid : NULL;
    }
    const ssh_host* h = strcmp(name, "hosts") == 0 ? host(index) : NULL;
    if (h == NULL) return NULL;
    if (strcmp(field, "alias") == 0) return h->alias;
    if (strcmp(field, "hostname") == 0) return h->hostname;
    if (strcmp(field, "user") == 0) return h->user;
    if (strcmp(field, "current") == 0) return h == state->current_host ? "← 지금" : NULL;
    if (strcmp(field, "port") == 0) {
        snprintf(text, sizeof text, "%u", h->port);
        return text;
    }
    return NULL;
}

static size_t chosen(void* user, const char* name) {
    (void)user;
    size_t i = 0;
    if (strcmp(name, "hosts") == 0)
        for (const ssh_host* h = state->hosts; h != NULL && h != state->current_host; h = h->next)
            i++;
    return i;
}

static void get(void* user, const setting* s, setting_value* out) {
    (void)user;
    settings_get(state, s, out);
}

const ui_source* ui_values(const global_state* global) {
    static const ui_source source = {value, count, item, chosen, get, NULL};
    state = global;
    return &source;
}
