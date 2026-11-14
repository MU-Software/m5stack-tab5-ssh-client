// SPDX-License-Identifier: GPL-3.0-or-later
// Times: microseconds since boot, as esp_timer counts
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define NETWORK_SECOND 1000000LL
#define NETWORK_NEVER  INT64_MAX

typedef enum { NETWORK_NONE, NETWORK_WIFI, NETWORK_LTE, NETWORK_PATHS } network_path;
typedef enum { NETWORK_UNKNOWN, NETWORK_GOOD, NETWORK_BAD } network_health;
typedef enum { NETWORK_FINE, NETWORK_HOST_DOWN, NETWORK_NO_NAME, NETWORK_NO_INTERNET } network_trouble;

typedef struct {
    // This boot, and the C6 checks the internet itself
    bool enabled;
    bool c6_heard;
    // C6_WIFI_*
    uint8_t c6_state;
    int8_t rssi;
    char ssid[33];
    // Joined, with the lease on the interface
    bool link;
    // A join this chip asked for runs
    bool planned;
    // +1 when a link-up brings another address or network
    unsigned epoch;
    // Unknown at each link-up
    network_health internet;
    int64_t started_at, linked_at, lost_at, internet_at, good_since, asked_at;
    // The last two drops or internet losses nobody asked for
    int64_t failed_at, failed_before;
} wifi_view;

typedef enum { LTE_OFF, LTE_NO_MODEM, LTE_DIALING, LTE_REGISTERING, LTE_WAITING, LTE_UP } lte_state;
typedef struct {
    lte_state state;
    // Unknown and +1 at each PPP up
    network_health internet;
    unsigned epoch;
    // received_at: an LTE session's last bytes
    int64_t up_at, internet_at, received_at;
} lte_view;

typedef struct {
    network_path path;
    // A Wi-Fi rejoining after a drop counts
    bool usable[NETWORK_PATHS];
    unsigned epoch[NETWORK_PATHS];
} network_choice;

network_path network_other(network_path path);
bool wifi_good(const wifi_view* wifi);
// The drop may reach this chip before the C6's state that follows it
bool wifi_rejoining(const wifi_view* wifi, int64_t now);
network_choice network_choose(const wifi_view* wifi, const lte_view* lte, network_path previous, int64_t now);

// pinned: not the chosen path, as only it found the host
typedef struct {
    network_path path;
    unsigned epoch;
    bool pinned;
} network_use;
bool network_session_moves(const network_use* use, const network_choice* choice);

typedef enum { NETWORK_FOUND, NETWORK_NOT_FOUND, NETWORK_NO_ANSWER } network_lookup;
bool network_ask_other(const network_choice* choice, network_lookup on_chosen, bool chosen_checked);
network_use network_host_path(const network_choice* choice, network_lookup on_chosen, network_lookup on_other);

network_trouble network_judge(bool named, bool online);
const char* network_trouble_text(network_trouble trouble);

int64_t wifi_ask_at(const wifi_view* wifi, bool suspected);
bool wifi_c6_silent(const wifi_view* wifi, int64_t now);

int64_t lte_check_at(const lte_view* lte, int64_t first_failed_at, int64_t checked_at, bool suspected, int64_t now);
bool lte_bearer_dead(int64_t up_at, int64_t first_failed_at, int64_t failed_at, int64_t received_at);

typedef enum { LTE_ASKED, LTE_UP_ENDED, LTE_BEARER_DEAD, LTE_BEARER_REMADE, LTE_NEVER_ANSWERED, LTE_SILENT, LTE_REFUSED, LTE_NOT_READY } lte_outcome;
// last_dial_at is the caller's, set at each dial
typedef struct {
    int drops, deaths;
    int64_t last_dial_at, last_death_at;
} lte_dial_memory;
int64_t lte_dial_at(lte_outcome outcome, int64_t up_for, lte_dial_memory* memory, int64_t now);
