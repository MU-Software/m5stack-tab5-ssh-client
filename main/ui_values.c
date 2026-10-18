// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_values.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <esp_attr.h>
#include <esp_timer.h>

#include "c6_protocol.h"
#include "clock.h"
#include "lte.h"
#include "network.h"
#include "wifi.h"
#include "power.h"
#include "screen.h"

#define SECOND 1000000LL

static const global_state* state;
static network_info net;
static int millivolts = -1;
static bool charging;
EXT_RAM_BSS_ATTR static modem_snapshot modem;
// One value at a time: the UI copies each before asking for the next
static char text[96];

static const char* const lte_states[] = {
    [LTE_OFF] = "꺼짐",
    [LTE_NO_MODEM] = "모뎀 없음",
    [LTE_DIALING] = "다이얼 중",
    [LTE_REGISTERING] = "등록 중",
    [LTE_WAITING] = "대기, 다시 시도 예정",
    [LTE_UP] = "연결됨",
};

// The network every 2 s (it asks the C6), the battery every 5 s
static void refresh(void) {
    static int64_t described = -2 * SECOND, measured = -5 * SECOND;
    int64_t now = esp_timer_get_time();
    if (now - described >= 2 * SECOND) {
        network_describe(&net);
        lte_snapshot(&modem);
        described = now;
    }
    if (now - measured >= 5 * SECOND) {
        if (!power_battery(&millivolts, &charging)) millivolts = -1;
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

static const char* const session_states[] = {
    [SESSION_DISCONNECTED] = "끊김",         [SESSION_CONNECTING] = "연결 중", [SESSION_CONNECTED] = "연결됨",
    [SESSION_RECONNECTING] = "다시 연결 중", [SESSION_ASKING] = "입력 기다림", [SESSION_DELETING] = "삭제 중",
};

// A session's tab: ● for the shown one, and a mark for one that is not connected or has a failure to tell
static const char* tab(int number) {
    static const char* const marks[] = {
        [SESSION_DISCONNECTED] = " ×", [SESSION_CONNECTING] = " …", [SESSION_CONNECTED] = "",
        [SESSION_RECONNECTING] = " …", [SESSION_ASKING] = " ?",     [SESSION_DELETING] = " ×",
    };
    for (size_t i = 0; i < state->session_count; i++) {
        const session_info* s = &state->sessions[i];
        if (s->number != number) continue;
        snprintf(text, sizeof text, "%s %d %.48s%s", s->shown ? "●" : "○", number, s->alias, s->alert ? " !" : marks[s->status]);
        return text;
    }
    return NULL;
}

static const char* network(void) {
    if (strcmp(net.path, "Wi-Fi") == 0)
        snprintf(text, sizeof text, "Wi-Fi %s", net.ssid);
    else if (strcmp(net.path, "LTE") == 0 && net.wifi_joined && net.wifi_internet == NETWORK_BAD)
        snprintf(text, sizeof text, "LTE · %s 인터넷 없음", net.ssid);
    else if (strcmp(net.path, "LTE") == 0)
        snprintf(text, sizeof text, "LTE");
    else
        snprintf(text, sizeof text, "망 없음 · LTE %s", net.lte_on ? lte_states[net.lte] : "꺼짐");
    return text;
}

// {modem.NAME}, and {modem.NAME.age} for when it was read
static const char* modem_shown(const char* name) {
    char shown[32], scratch[96];
    if (strcmp(name, "read") == 0)
        return net.lte_address[0] != 0 ? "PPP 중: 다이얼 전에 읽은 값 · 다시 읽으려면 LTE 끄기" : "PPP 아님: 다시 읽기로 새로 읽음";
    const char* age = strstr(name, ".age");
    if (age == NULL) return modem_text(&modem, name, text, sizeof text) != MODEM_ITEMS ? text : NULL;
    snprintf(shown, sizeof shown, "%.*s", (int)(age - name), name);
    modem_item item = modem_text(&modem, shown, scratch, sizeof scratch);
    if (item == MODEM_ITEMS || modem.read_at[item] == 0) return NULL;
    int64_t ago = (esp_timer_get_time() - modem.read_at[item]) / SECOND;
    if (ago < 120)
        snprintf(text, sizeof text, "%d초 전", (int)ago);
    else
        snprintf(text, sizeof text, "%d분 전", (int)(ago / 60));
    return text;
}

// Forgotten networks keep their place with an empty SSID
static const wifi_network* network_at(size_t index) {
    for (const wifi_network* w = state->wifi; w != NULL; w = w->next)
        if (w->ssid[0] != 0 && index-- == 0) return w;
    return NULL;
}

static const char* wifi_value(const char* name) {
    if (strcmp(name, "path") == 0) return strcmp(net.path, "Wi-Fi") == 0 ? "●" : NULL;
    if (strcmp(name, "toggle") == 0) return state->wifi_enabled ? "끄기" : "켜기";
    if (strcmp(name, "adding") == 0) return state->wifi_adding ? "1" : NULL;
    if (strcmp(name, "count") == 0) {
        size_t saved = 0;
        while (network_at(saved) != NULL)
            saved++;
        snprintf(text, sizeof text, "%zu/%d", saved, C6_NETWORKS);
        return text;
    }
    if (strcmp(name, "scan") == 0) {
        wifi_visible_network v;
        wifi_scan_state scan = wifi_scan_now();
        return !net.wifi_on                                                  ? "Wi-Fi가 꺼져 있음"
               : scan == WIFI_SCAN_RUNNING                                   ? "찾는 중…"
               : scan == WIFI_SCAN_FAILED                                    ? "찾지 못함"
               : scan == WIFI_SCAN_DONE && !wifi_visible(state->wifi, 0, &v) ? "사용 가능한 Wi-Fi 없음"
                                                                             : NULL;
    }
    return NULL;
}

static const char* value(void* user, const char* name) {
    (void)user;
    refresh();
    int number, end = 0;
    if (sscanf(name, "session.%d%n", &number, &end) == 1 && name[end] == 0) return tab(number);
    if (strcmp(name, "sessions.none") == 0) return state->session_count == 0 ? "1" : NULL;
    if (strcmp(name, "sessions.any") == 0) return state->session_count > 0 ? "1" : NULL;
    if (strcmp(name, "hosts.room") == 0) return state->hosts != NULL && state->session_count < MAX_SESSIONS ? "1" : NULL;
    if (strcmp(name, "sessions") == 0) {
        snprintf(text, sizeof text, "%zu/%d", state->session_count, MAX_SESSIONS);
        return text;
    }
    if (strcmp(name, "status") == 0) return screen_status(text, sizeof text) ? text : NULL;
    if (strcmp(name, "network") == 0) return network();
    if (strcmp(name, "battery") == 0) {
        if (millivolts < 0) return NULL;
        const char* mark = charging ? "⚡" : "";
        if (state->battery == 1)
            snprintf(text, sizeof text, "%s%d%%", mark, percent(millivolts));
        else
            snprintf(text, sizeof text, "%s%d.%02d V", mark, millivolts / 1000, millivolts % 1000 / 10);
        return text;
    }
    if (strcmp(name, "time") == 0) {
        time_t now = time(NULL);
        struct tm local;
        if (!clock_known()) return "--:--";
        strftime(text, sizeof text, "%H:%M", localtime_r(&now, &local));
        return text;
    }
    if (strcmp(name, "wifi.state") == 0) {
        if (!net.wifi_on) return "꺼짐";
        if (!net.wifi_joined) return "찾는 중";
        snprintf(text, sizeof text, "%s %s", net.ssid,
                 (const char* const[]){"연결됨, 인터넷 확인 중", "연결됨", "연결됨, 인터넷 없음"}[net.wifi_internet]);
        return text;
    }
    if (strcmp(name, "wifi.detail") == 0) {
        if (!net.wifi_joined) return NULL;
        snprintf(text, sizeof text, "%s · %d dBm", net.wifi_address, net.rssi);
        return text;
    }
    if (strcmp(name, "lte.state") == 0) return net.lte_on ? lte_states[net.lte] : "꺼짐";
    if (strcmp(name, "lte.path") == 0) return strcmp(net.path, "LTE") == 0 ? "●" : NULL;
    if (strcmp(name, "lte.toggle") == 0) return state->lte_enabled ? "끄기" : "켜기";
    if (strcmp(name, "lte.detail") == 0) return net.lte_address[0] != 0 ? net.lte_address : NULL;
    if (strncmp(name, "modem.", 6) == 0) return modem_shown(name + 6);
    if (strncmp(name, "wifi.", 5) == 0) return wifi_value(name + 5);
    if (strcmp(name, "config") == 0) {
        static const char* const problems[] = {
            [CONFIG_LOADED] = NULL,
            [CONFIG_MISSING] = "SD 카드에 .config/tab5-ssh/config.json 없음 · 기본값으로 실행, 바꾸면 만듦",
            [CONFIG_NO_CARD] = "SD 카드 없음 · 기본값으로 실행, 바꾼 설정은 저장 안 됨",
            [CONFIG_INVALID] = "config.json이 JSON이 아님 · 기본값으로 실행, 고칠 때까지 저장 안 됨",
        };
        return problems[state->config];
    }
    return NULL;
}

static const ssh_host* host(size_t index) {
    const ssh_host* h = state->hosts;
    for (; h != NULL && index > 0; index--)
        h = h->next;
    return h;
}

static size_t count(void* user, const char* name) {
    (void)user;
    size_t n = 0;
    wifi_visible_network v;
    if (strcmp(name, "visible") == 0)
        while (wifi_visible(state->wifi, n, &v))
            n++;
    if (strcmp(name, "hosts") == 0)
        while (host(n) != NULL)
            n++;
    if (strcmp(name, "wifi") == 0)
        while (network_at(n) != NULL)
            n++;
    if (strcmp(name, "sessions") == 0) n = state->session_count;
    return n;
}

static const char* item(void* user, const char* name, size_t index, const char* field) {
    (void)user;
    if (strcmp(name, "wifi") == 0) {
        const wifi_network* w = network_at(index);
        if (w == NULL) return NULL;
        bool joined = net.wifi_joined && strcmp(w->ssid, net.ssid) == 0;
        if (strcmp(field, "ssid") == 0) return w->ssid;
        if (strcmp(field, "joined") == 0) return joined ? "연결됨" : NULL;
        if (strcmp(field, "join") == 0) return joined ? NULL : "1";
        if (strcmp(field, "up") == 0) return index > 0 ? "1" : NULL;
        if (strcmp(field, "order") != 0) return NULL;
        snprintf(text, sizeof text, "%zu", index + 1);
        return text;
    }
    const session_info* s = strcmp(name, "sessions") == 0 && index < state->session_count ? &state->sessions[index] : NULL;
    if (s != NULL) {
        if (strcmp(field, "alias") == 0) return s->alias;
        if (strcmp(field, "status") == 0) return session_states[s->status];
        if (strcmp(field, "toggle") == 0) return s->status == SESSION_DISCONNECTED ? "연결" : "끊기";
        if (strcmp(field, "idle") == 0) return s->status != SESSION_DELETING && !s->creating ? "1" : NULL;
        if (strcmp(field, "number") != 0) return NULL;
        snprintf(text, sizeof text, "%d", s->number);
        return text;
    }
    wifi_visible_network v;
    if (strcmp(name, "visible") == 0 && wifi_visible(state->wifi, index, &v)) {
        if (strcmp(field, "ssid") == 0) snprintf(text, sizeof text, "%s", v.ssid);
        if (strcmp(field, "signal") == 0) snprintf(text, sizeof text, "%d dBm", v.rssi);
        if (strcmp(field, "lock") == 0) return v.open ? "열림" : NULL;
        return strcmp(field, "ssid") == 0 || strcmp(field, "signal") == 0 ? text : NULL;
    }
    const ssh_host* h = strcmp(name, "hosts") == 0 ? host(index) : NULL;
    if (h == NULL) return NULL;
    if (strcmp(field, "alias") == 0) return h->alias;
    if (strcmp(field, "hostname") == 0) return h->hostname;
    if (strcmp(field, "user") == 0) return h->user;
    if (strcmp(field, "port") == 0 && h->port != 22) {
        snprintf(text, sizeof text, ":%u", h->port);
        return text;
    }
    return NULL;
}

static size_t chosen(void* user, const char* name) {
    (void)user;
    size_t i = 0;
    if (strcmp(name, "hosts") == 0)
        for (const ssh_host* h = state->hosts; h != NULL && state->current_host != NULL && h != state->current_host; h = h->next)
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
