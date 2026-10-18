// SPDX-License-Identifier: GPL-3.0-or-later
#include "network_rules.h"

#include <stddef.h>

#include "c6_protocol.h"

#define MINUTE (60 * NETWORK_SECOND)

static int64_t later(int64_t a, int64_t b) {
    return a > b ? a : b;
}

network_path network_other(network_path p) {
    return p == NETWORK_WIFI ? NETWORK_LTE : p == NETWORK_LTE ? NETWORK_WIFI : NETWORK_NONE;
}

bool wifi_good(const wifi_view* w) {
    return w->enabled && w->link && w->internet == NETWORK_GOOD;
}

bool wifi_rejoining(const wifi_view* w, int64_t now) {
    bool final = w->c6_heard && (w->c6_state == C6_WIFI_NONE_IN_RANGE || w->c6_state == C6_WIFI_JOIN_FAILED || w->c6_state == C6_WIFI_STOPPED);
    return w->enabled && !w->link && w->lost_at != 0 && now - w->lost_at < 10 * NETWORK_SECOND && !final;
}

network_choice network_choose(const wifi_view* w, const lte_view* l, network_path previous, int64_t now) {
    bool rejoining = wifi_rejoining(w, now);
    // The C6 checks a new link within 9 s
    bool checking = w->enabled && w->link && w->internet == NETWORK_UNKNOWN && now - w->linked_at < 10 * NETWORK_SECOND;
    bool planned = w->enabled && w->planned && now - w->lost_at < 25 * NETWORK_SECOND;
    bool good = wifi_good(w);
    bool lte_good = l->state == LTE_UP && l->internet != NETWORK_BAD;
    // Two failures within 10 min, the last of them recent, and the internet back for less than 30 s
    bool flapping = w->failed_before != 0 && w->failed_at - w->failed_before < 10 * MINUTE && now - w->failed_at < 10 * MINUTE &&
                    now - w->good_since < 30 * NETWORK_SECOND;
    bool first_try = w->enabled && now - w->started_at < 20 * NETWORK_SECOND &&
                     (!w->c6_heard || w->c6_state == C6_WIFI_SCANNING || w->c6_state == C6_WIFI_JOINING || checking);
    network_choice c = {.usable = {false, good || rejoining || checking || planned, lte_good}, .epoch = {0, w->epoch, l->epoch}};
    if ((good && !(previous == NETWORK_LTE && lte_good && flapping)) || (previous == NETWORK_WIFI && (rejoining || checking || planned)))
        c.path = NETWORK_WIFI;
    else if (previous != NETWORK_LTE && first_try)
        c.path = NETWORK_NONE;
    else
        c.path = lte_good ? NETWORK_LTE : NETWORK_NONE;
    return c;
}

bool network_session_moves(const network_use* use, const network_choice* c) {
    return use->path == NETWORK_NONE || c->epoch[use->path] != use->epoch || !c->usable[use->path] ||
           (!use->pinned && c->path != NETWORK_NONE && c->path != use->path);
}

bool network_ask_other(const network_choice* c, network_lookup on_chosen, bool chosen_checked) {
    return c->usable[network_other(c->path)] && (on_chosen == NETWORK_NOT_FOUND || (on_chosen == NETWORK_NO_ANSWER && chosen_checked));
}

network_use network_host_path(const network_choice* c, network_lookup on_chosen, network_lookup on_other) {
    network_path p = on_chosen != NETWORK_FOUND && on_other == NETWORK_FOUND ? network_other(c->path) : c->path;
    return (network_use){.path = p, .epoch = c->epoch[p], .pinned = p != c->path};
}

network_trouble network_judge(bool named, bool online) {
    return !online ? NETWORK_NO_INTERNET : !named ? NETWORK_NO_NAME : NETWORK_HOST_DOWN;
}

const char* network_trouble_text(network_trouble trouble) {
    static const char* const texts[] = {
        [NETWORK_FINE] = NULL,
        [NETWORK_HOST_DOWN] = "reconnecting: the host does not answer",
        [NETWORK_NO_NAME] = "reconnecting: the host's name does not resolve",
        [NETWORK_NO_INTERNET] = "reconnecting: no internet",
    };
    return (unsigned)trouble < sizeof texts / sizeof *texts ? texts[trouble] : NULL;
}

int64_t wifi_ask_at(const wifi_view* w, bool suspected) {
    if (!w->enabled || !w->link) return NETWORK_NEVER;
    if (suspected) return w->asked_at < w->linked_at ? w->linked_at : w->asked_at + 30 * NETWORK_SECOND;
    if (w->internet != NETWORK_UNKNOWN || w->asked_at >= w->linked_at) return NETWORK_NEVER;
    // A link taken over at start (this chip restarted) brings no check of its own
    return w->linked_at - w->started_at < NETWORK_SECOND ? w->linked_at : w->linked_at + 3 * NETWORK_SECOND;
}

bool wifi_c6_silent(const wifi_view* w, int64_t now) {
    return w->asked_at != 0 && w->internet_at < w->asked_at && now - w->asked_at >= 17 * NETWORK_SECOND;
}

int64_t lte_check_at(const lte_view* l, int64_t first_failed_at, int64_t checked_at, bool suspected, int64_t now) {
    if (l->state != LTE_UP || l->received_at >= now - 10 * NETWORK_SECOND) return NETWORK_NEVER;
    if (first_failed_at != 0) return later(first_failed_at + 30 * NETWORK_SECOND, l->up_at + 60 * NETWORK_SECOND);
    if (checked_at < l->up_at || l->internet != NETWORK_GOOD || (suspected && l->internet_at < now - 2 * MINUTE)) return now;
    return l->internet_at + 10 * MINUTE;
}

bool lte_bearer_dead(int64_t up_at, int64_t first_failed_at, int64_t failed_at, int64_t received_at) {
    return first_failed_at != 0 && first_failed_at - up_at >= 60 * NETWORK_SECOND && failed_at - first_failed_at >= 30 * NETWORK_SECOND &&
           received_at < first_failed_at;
}

int64_t lte_dial_at(lte_outcome outcome, int64_t up_for, int* drops, int* deaths, int64_t last_death_at, int64_t last_dial_at, int64_t now) {
    static const int64_t after_death[] = {0, 10 * MINUTE, 30 * MINUTE, 60 * MINUTE};
    int64_t at = now;
    switch (outcome) {
    case LTE_ASKED:
        *drops = 0;
        break;
    case LTE_UP_ENDED:
        if (up_for >= 960 * NETWORK_SECOND) *drops = 0;
        at = now + (120 * NETWORK_SECOND << *drops);
        if (*drops < 3) (*drops)++;
        break;
    case LTE_BEARER_DEAD:
        if (now - last_death_at >= 120 * MINUTE) *deaths = 0;
        at = now + after_death[*deaths < 3 ? *deaths : 3];
        (*deaths)++;
        break;
    case LTE_BEARER_REMADE:
        break;
    case LTE_NEVER_ANSWERED:
        return NETWORK_NEVER;
    case LTE_SILENT:
    case LTE_REFUSED:
        at = now + 960 * NETWORK_SECOND;
        break;
    case LTE_NOT_READY:
        at = now + 90 * NETWORK_SECOND;
        break;
    }
    return later(at, last_dial_at + 60 * NETWORK_SECOND);
}
