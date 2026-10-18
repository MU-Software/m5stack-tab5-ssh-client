// SPDX-License-Identifier: GPL-3.0-or-later
// The network's decisions from what Wi-Fi and LTE report; times in microseconds since boot, as esp_timer counts
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define NETWORK_SECOND 1000000LL
#define NETWORK_NEVER  INT64_MAX

typedef enum { NETWORK_NONE, NETWORK_WIFI, NETWORK_LTE, NETWORK_PATHS } network_path;
typedef enum { NETWORK_UNKNOWN, NETWORK_GOOD, NETWORK_BAD } network_health;
// Why a host could not be reached over a path
typedef enum { NETWORK_FINE, NETWORK_HOST_DOWN, NETWORK_NO_NAME, NETWORK_NO_INTERNET } network_trouble;

typedef struct {
    // This boot, with a C6 that checks the internet itself
    bool enabled;
    // A C6_WIFI_STATE came this boot
    bool c6_heard;
    // C6_WIFI_* of c6_protocol.h
    uint8_t c6_state;
    int8_t rssi;
    char ssid[33];
    // The C6 is joined and the interface holds its lease
    bool link;
    // A join asked for runs
    bool planned;
    // +1 when the address held or the network at a link-up changes
    unsigned epoch;
    // Unknown at each link-up, then from the C6's checks begun after it
    network_health internet;
    // Wi-Fi started, the link came, it went, the last internet result, the internet found since, the last ask
    int64_t started_at, linked_at, lost_at, internet_at, good_since, asked_at;
    // The last two drops and losses of the internet nobody asked for
    int64_t failed_at, failed_before;
} wifi_view;

typedef enum { LTE_OFF, LTE_NO_MODEM, LTE_DIALING, LTE_REGISTERING, LTE_WAITING, LTE_UP } lte_state;
typedef struct {
    // LTE_UP: PPP up with an address
    lte_state state;
    // Unknown at each PPP up
    network_health internet;
    // +1 at each PPP up
    unsigned epoch;
    // PPP up, the last check's result, the last bytes an LTE session received
    int64_t up_at, internet_at, received_at;
} lte_view;

typedef struct {
    network_path path;
    // Per path, a Wi-Fi rejoining after a drop included
    bool usable[NETWORK_PATHS];
    unsigned epoch[NETWORK_PATHS];
} network_choice;

network_path network_other(network_path path);
// Joined with the internet
bool wifi_good(const wifi_view* wifi);
// After a drop, until the C6 gives up on rejoining or 10 s passed: the drop may reach this chip before the C6's state that follows
bool wifi_rejoining(const wifi_view* wifi, int64_t now);
// Wi-Fi with the internet, but back from LTE only to Wi-Fi that stays; Wi-Fi kept while the C6 rejoins or checks a new link;
// no LTE while the C6 makes its first try after a boot; then LTE
network_choice network_choose(const wifi_view* wifi, const lte_view* lte, network_path previous, int64_t now);

// What a session connected over; pinned: a path other than the chosen one, as only it found the host
typedef struct {
    network_path path;
    unsigned epoch;
    bool pinned;
} network_use;
// Its path gone, renewed (a new address) or not chosen
bool network_session_moves(const network_use* use, const network_choice* choice);

typedef enum { NETWORK_FOUND, NETWORK_NOT_FOUND, NETWORK_NO_ANSWER } network_lookup;
// The other path is asked for a name the chosen one did not find: one its DNS denied, or one none answered for while it reaches
// the internet (checked since its link came)
bool network_ask_other(const network_choice* choice, network_lookup on_chosen, bool chosen_checked);
// The path that carries a host, from what each path's DNS found
network_use network_host_path(const network_choice* choice, network_lookup on_chosen, network_lookup on_other);

// A host not reached: its name found, its path's internet
network_trouble network_judge(bool named, bool online);
// What a session waiting to reconnect shows, NULL for NETWORK_FINE
const char* network_trouble_text(network_trouble trouble);

// When the P4 asks the C6 to check its network: a link with no result 3 s after it came, or at once for one taken over at start,
// or one under suspicion, at most every 30 s
int64_t wifi_ask_at(const wifi_view* wifi, bool suspected);
// Asked 17 s ago with no answer since
bool wifi_c6_silent(const wifi_view* wifi, int64_t now);

// When LTE is checked, chosen or not: after each PPP up; 30 s after a failure (60 s into the link at the earliest) until a verdict;
// on suspicion unless good within 2 min; 10 min after the last good result. Never while LTE sessions receive
int64_t lte_check_at(const lte_view* lte, int64_t first_failed_at, int64_t checked_at, bool suspected, int64_t now);
// Two failed checks 30 s apart, the first 60 s into the link, nothing received since it
bool lte_bearer_dead(int64_t up_at, int64_t first_failed_at, int64_t failed_at, int64_t received_at);

typedef enum { LTE_ASKED, LTE_UP_ENDED, LTE_BEARER_DEAD, LTE_BEARER_REMADE, LTE_NEVER_ANSWERED, LTE_SILENT, LTE_REFUSED, LTE_NOT_READY } lte_outcome;
// When the next dial may go, never within 60 s of the last: lte on at once; after a link 120 s doubling to 960 s (the doubling
// forgotten after 960 s up); after a dead bearer at once, then 10, 30 and 60 min (forgotten after 2 h without one), but at once
// uncounted when the network made a new one; a modem silent
// or refusing 960 s; not ready 90 s; one that never answered when it speaks
int64_t lte_dial_at(lte_outcome outcome, int64_t up_for, int* drops, int* deaths, int64_t last_death_at, int64_t last_dial_at, int64_t now);
