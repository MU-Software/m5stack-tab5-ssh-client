// SPDX-License-Identifier: GPL-3.0-or-later
#include "network_rules.h"

#include <stdio.h>

#include "c6_protocol.h"

#define S(x) ((int64_t)(x) * NETWORK_SECOND)

static int failures;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

#define ON  true
#define OFF false

// Seconds since boot; Wi-Fi started at 1 s, epoch 1
typedef struct {
    const char* what;
    bool on, heard, link, planned;
    uint8_t state;
    network_health internet;
    int linked, lost, before;
    lte_state lte;
    network_health lte_internet;
    network_path previous;
    int now;
    network_path want;
} choose_case;

enum { NO = 0 };
#define JOINED         ON, ON, ON, OFF, C6_WIFI_CONNECTED
#define DROPPED(state) ON, ON, OFF, OFF, state

static const choose_case choose_cases[] = {
    {"B1 boot, no C6 state yet: no LTE", ON, OFF, OFF, OFF, 0, NETWORK_UNKNOWN, NO, NO, NO, LTE_UP, NETWORK_UNKNOWN, NETWORK_NONE, 3, NETWORK_NONE},
    {"B1 boot, C6 joining: no LTE", ON, ON, OFF, OFF, C6_WIFI_JOINING, NETWORK_UNKNOWN, NO, NO, NO, LTE_UP, NETWORK_UNKNOWN, NETWORK_NONE, 8,
     NETWORK_NONE},
    {"B1 lease, C6 checking: none yet", JOINED, NETWORK_UNKNOWN, 5, NO, NO, LTE_OFF, NETWORK_UNKNOWN, NETWORK_NONE, 6, NETWORK_NONE},
    {"B1 lease, C6 good: Wi-Fi", JOINED, NETWORK_GOOD, 5, NO, NO, LTE_OFF, NETWORK_UNKNOWN, NETWORK_NONE, 7, NETWORK_WIFI},
    {"B2 none in range ends the first try", DROPPED(C6_WIFI_NONE_IN_RANGE), NETWORK_UNKNOWN, NO, NO, NO, LTE_UP, NETWORK_UNKNOWN, NETWORK_NONE, 8,
     NETWORK_LTE},
    {"B2 first try over after 20 s", DROPPED(C6_WIFI_SCANNING), NETWORK_UNKNOWN, NO, NO, NO, LTE_UP, NETWORK_UNKNOWN, NETWORK_NONE, 21, NETWORK_LTE},
    {"B3 LTE chosen first stays when the C6 starts trying", DROPPED(C6_WIFI_SCANNING), NETWORK_UNKNOWN, NO, NO, NO, LTE_UP, NETWORK_UNKNOWN,
     NETWORK_LTE, 5, NETWORK_LTE},
    {"B6 Wi-Fi off: LTE at once", OFF, OFF, OFF, OFF, 0, NETWORK_UNKNOWN, NO, NO, NO, LTE_UP, NETWORK_UNKNOWN, NETWORK_NONE, 3, NETWORK_LTE},
    {"W3 drop seen before the C6's state: Wi-Fi kept", DROPPED(C6_WIFI_CONNECTED), NETWORK_GOOD, 5, 100, NO, LTE_UP, NETWORK_GOOD, NETWORK_WIFI, 100,
     NETWORK_WIFI},
    {"W3 C6 held during a scan: Wi-Fi kept", DROPPED(C6_WIFI_HELD), NETWORK_GOOD, 5, 100, NO, LTE_UP, NETWORK_GOOD, NETWORK_WIFI, 103, NETWORK_WIFI},
    {"W3 C6 joining 9 s after: Wi-Fi kept", DROPPED(C6_WIFI_JOINING), NETWORK_GOOD, 5, 100, NO, LTE_UP, NETWORK_GOOD, NETWORK_WIFI, 109,
     NETWORK_WIFI},
    {"W3 10 s after: LTE", DROPPED(C6_WIFI_JOINING), NETWORK_GOOD, 5, 100, NO, LTE_UP, NETWORK_GOOD, NETWORK_WIFI, 110, NETWORK_LTE},
    {"W3 back, C6 checking: Wi-Fi kept", JOINED, NETWORK_UNKNOWN, 108, 100, NO, LTE_UP, NETWORK_GOOD, NETWORK_WIFI, 116, NETWORK_WIFI},
    {"W3 back, not checked in 10 s: LTE", JOINED, NETWORK_UNKNOWN, 108, 100, NO, LTE_UP, NETWORK_GOOD, NETWORK_WIFI, 118, NETWORK_LTE},
    {"W3 back, C6 good: Wi-Fi", JOINED, NETWORK_GOOD, 108, 100, NO, LTE_UP, NETWORK_GOOD, NETWORK_WIFI, 118, NETWORK_WIFI},
    {"W5 none in range ends the grace", DROPPED(C6_WIFI_NONE_IN_RANGE), NETWORK_GOOD, 5, 100, NO, LTE_UP, NETWORK_GOOD, NETWORK_WIFI, 101,
     NETWORK_LTE},
    {"W5 nothing else: none", DROPPED(C6_WIFI_JOIN_FAILED), NETWORK_GOOD, 5, 100, NO, LTE_OFF, NETWORK_UNKNOWN, NETWORK_WIFI, 101, NETWORK_NONE},
    {"W6 no internet: LTE", JOINED, NETWORK_BAD, 5, NO, NO, LTE_UP, NETWORK_UNKNOWN, NETWORK_WIFI, 50, NETWORK_LTE},
    {"W6 no internet, no LTE: none", JOINED, NETWORK_BAD, 5, NO, NO, LTE_WAITING, NETWORK_UNKNOWN, NETWORK_WIFI, 50, NETWORK_NONE},
    {"W11 back from LTE to Wi-Fi that did not flap: at once", JOINED, NETWORK_GOOD, 200, 150, NO, LTE_UP, NETWORK_GOOD, NETWORK_LTE, 201,
     NETWORK_WIFI},
    {"W11 flapping: LTE kept for 30 s", JOINED, NETWORK_GOOD, 200, 150, 100, LTE_UP, NETWORK_GOOD, NETWORK_LTE, 229, NETWORK_LTE},
    {"W11 flapping, stayed 30 s: Wi-Fi", JOINED, NETWORK_GOOD, 200, 150, 100, LTE_UP, NETWORK_GOOD, NETWORK_LTE, 230, NETWORK_WIFI},
    {"W11 flapping but LTE gone: Wi-Fi", JOINED, NETWORK_GOOD, 200, 150, 100, LTE_WAITING, NETWORK_UNKNOWN, NETWORK_LTE, 201, NETWORK_WIFI},
    {"W11 drops long ago are no flapping", JOINED, NETWORK_GOOD, 900, 150, 100, LTE_UP, NETWORK_GOOD, NETWORK_LTE, 901, NETWORK_WIFI},
    {"W11 drops 10 min apart are no flapping", JOINED, NETWORK_GOOD, 800, 700, 100, LTE_UP, NETWORK_GOOD, NETWORK_LTE, 801, NETWORK_WIFI},
    {"W13 wifi off: LTE at once", OFF, ON, ON, OFF, C6_WIFI_CONNECTED, NETWORK_GOOD, 5, NO, NO, LTE_UP, NETWORK_GOOD, NETWORK_WIFI, 50, NETWORK_LTE},
    {"W15 planned join, 20 s after the drop: Wi-Fi kept", ON, ON, OFF, ON, C6_WIFI_JOIN_FAILED, NETWORK_GOOD, 5, 100, NO, LTE_UP, NETWORK_GOOD,
     NETWORK_WIFI, 120, NETWORK_WIFI},
    {"W15 planned join, 25 s: LTE", ON, ON, OFF, ON, C6_WIFI_JOINING, NETWORK_GOOD, 5, 100, NO, LTE_UP, NETWORK_GOOD, NETWORK_WIFI, 125, NETWORK_LTE},
    {"LTE bad: none", OFF, OFF, OFF, OFF, 0, NETWORK_UNKNOWN, NO, NO, NO, LTE_UP, NETWORK_BAD, NETWORK_LTE, 50, NETWORK_NONE},
    {"LTE unknown is used", OFF, OFF, OFF, OFF, 0, NETWORK_UNKNOWN, NO, NO, NO, LTE_UP, NETWORK_UNKNOWN, NETWORK_NONE, 50, NETWORK_LTE},
    {"LTE dialing: none", OFF, OFF, OFF, OFF, 0, NETWORK_UNKNOWN, NO, NO, NO, LTE_DIALING, NETWORK_UNKNOWN, NETWORK_NONE, 50, NETWORK_NONE},
};

static void choose(void) {
    char what[128];
    for (size_t i = 0; i < sizeof choose_cases / sizeof *choose_cases; i++) {
        const choose_case* c = &choose_cases[i];
        wifi_view w = {.enabled = c->on,
                       .c6_heard = c->heard,
                       .c6_state = c->state,
                       .link = c->link,
                       .planned = c->planned,
                       .epoch = 1,
                       .internet = c->internet,
                       .started_at = S(1),
                       .linked_at = S(c->linked),
                       .good_since = c->internet == NETWORK_GOOD ? S(c->linked) : 0,
                       .lost_at = S(c->lost),
                       .failed_at = S(c->lost),
                       .failed_before = S(c->before)};
        lte_view l = {.state = c->lte, .internet = c->lte_internet, .epoch = 1, .up_at = S(30)};
        network_path got = network_choose(&w, &l, c->previous, S(c->now)).path;
        snprintf(what, sizeof what, "%s (got %d)", c->what, got);
        check(got == c->want, what);
    }
}

static wifi_view joined(void) {
    return (wifi_view){
        .enabled = true, .c6_heard = true, .c6_state = C6_WIFI_CONNECTED, .link = true, .epoch = 1, .started_at = S(1), .linked_at = S(5)};
}

// One break, not two: a Wi-Fi session through a short drop with the same lease, and through a move to another network
static void sessions(void) {
    wifi_view w = joined();
    w.internet = NETWORK_GOOD;
    lte_view l = {.state = LTE_UP, .epoch = 1, .up_at = S(30)};
    network_choice c = network_choose(&w, &l, NETWORK_NONE, S(10));
    network_use use = {.path = c.path, .epoch = c.epoch[c.path]};
    check(c.path == NETWORK_WIFI && !network_session_moves(&use, &c), "session on Wi-Fi");
    w.link = false, w.failed_at = w.lost_at = S(100);
    c = network_choose(&w, &l, c.path, S(100));
    check(c.path == NETWORK_WIFI && !network_session_moves(&use, &c), "W3 drop: the session stays");
    w.link = true, w.internet = NETWORK_UNKNOWN, w.linked_at = S(104);
    c = network_choose(&w, &l, c.path, S(105));
    check(c.path == NETWORK_WIFI && !network_session_moves(&use, &c), "W3 same lease back, checking: stays");
    w.internet = NETWORK_GOOD;
    c = network_choose(&w, &l, c.path, S(107));
    check(c.path == NETWORK_WIFI && !network_session_moves(&use, &c), "W3 checked: stays");
    w.link = false, w.failed_before = w.failed_at, w.failed_at = w.lost_at = S(200);
    c = network_choose(&w, &l, c.path, S(200));
    w.link = true, w.internet = NETWORK_UNKNOWN, w.linked_at = S(206), w.epoch = 2;
    c = network_choose(&w, &l, c.path, S(206));
    check(c.path == NETWORK_WIFI && network_session_moves(&use, &c), "W4 another network: the session moves once");
    use = (network_use){.path = c.path, .epoch = c.epoch[c.path]};
    w.internet = NETWORK_GOOD;
    c = network_choose(&w, &l, c.path, S(209));
    check(c.path == NETWORK_WIFI && !network_session_moves(&use, &c), "W4 checked: no second move");
    w.link = false, w.lost_at = S(300), w.c6_state = C6_WIFI_NONE_IN_RANGE;
    c = network_choose(&w, &l, c.path, S(301));
    check(c.path == NETWORK_LTE && network_session_moves(&use, &c), "W5 gone: to LTE");
    use = (network_use){.path = c.path, .epoch = c.epoch[c.path]};
    l.state = LTE_WAITING;
    c = network_choose(&w, &l, c.path, S(400));
    check(c.path == NETWORK_NONE && network_session_moves(&use, &c), "LTE gone too: the session ends");
    network_choice none = {.path = NETWORK_NONE, .usable = {false, false, true}, .epoch = {0, 2, 1}};
    check(!network_session_moves(&use, &none), "a change to none alone moves no session");
    network_choice wifi = {.path = NETWORK_WIFI, .usable = {false, true, true}, .epoch = {0, 2, 1}};
    check(network_session_moves(&use, &wifi), "back to Wi-Fi from LTE");
    use.pinned = true;
    check(!network_session_moves(&use, &wifi), "a pinned session stays on its path");
    use = (network_use){.path = NETWORK_WIFI, .epoch = 2, .pinned = true};
    network_choice rejoining = {.path = NETWORK_LTE, .usable = {false, true, true}, .epoch = {0, 2, 1}};
    check(!network_session_moves(&use, &rejoining), "a session pinned to Wi-Fi rides out its rejoin while LTE is chosen");
}

// The internet lost and found again on the same link counts as flapping too
static void verdicts(void) {
    wifi_view w = joined();
    lte_view l = {.state = LTE_UP, .epoch = 1, .up_at = S(30)};
    w.internet = NETWORK_GOOD, w.good_since = S(200), w.failed_at = S(190), w.failed_before = S(100);
    check(network_choose(&w, &l, NETWORK_LTE, S(210)).path == NETWORK_LTE, "W11 internet back on the same link after two losses: LTE kept");
    check(network_choose(&w, &l, NETWORK_LTE, S(230)).path == NETWORK_WIFI, "W11 and 30 s later: Wi-Fi");
}

static void hosts(void) {
    network_choice c = {.path = NETWORK_WIFI, .usable = {false, true, true}, .epoch = {0, 3, 7}};
    check(!network_ask_other(&c, NETWORK_FOUND, true), "found: nothing else");
    check(network_ask_other(&c, NETWORK_NOT_FOUND, false), "denied by the chosen path's DNS: the other is asked");
    check(network_ask_other(&c, NETWORK_NO_ANSWER, true), "no answer on a checked path: the other is asked");
    check(!network_ask_other(&c, NETWORK_NO_ANSWER, false), "no answer while the path rejoins or is unchecked: wait, pin nothing");
    c.usable[NETWORK_LTE] = false;
    check(!network_ask_other(&c, NETWORK_NOT_FOUND, true), "no other path");
    c.usable[NETWORK_LTE] = true;
    network_use u = network_host_path(&c, NETWORK_FOUND, NETWORK_NO_ANSWER);
    check(u.path == NETWORK_WIFI && u.epoch == 3 && !u.pinned, "found on the chosen path");
    u = network_host_path(&c, NETWORK_NOT_FOUND, NETWORK_FOUND);
    check(u.path == NETWORK_LTE && u.epoch == 7 && u.pinned, "found on the other only: pinned there");
    u = network_host_path(&c, NETWORK_NOT_FOUND, NETWORK_NOT_FOUND);
    check(u.path == NETWORK_WIFI && !u.pinned, "found nowhere: the chosen path");
}

static void judge(void) {
    check(network_judge(true, true) == NETWORK_HOST_DOWN, "resolved, internet, no answer: the host is down");
    check(network_judge(false, true) == NETWORK_NO_NAME, "internet but no name even from the public servers");
    check(network_judge(false, false) == NETWORK_NO_INTERNET, "no internet: the network");
    check(network_judge(true, false) == NETWORK_NO_INTERNET, "resolved but no internet: the network");
    check(network_trouble_text(NETWORK_FINE) == NULL && network_trouble_text(NETWORK_HOST_DOWN) != NULL, "texts for the waiting session");
}

static void wifi_asks(void) {
    wifi_view w = joined();
    check(wifi_ask_at(&w, false) == S(8), "a link with no result: asked 3 s after it came");
    w.linked_at = S(1) + NETWORK_SECOND / 2;
    check(wifi_ask_at(&w, false) == w.linked_at, "a link taken over at start: asked at once");
    w.linked_at = S(5);
    w.internet = NETWORK_GOOD;
    check(wifi_ask_at(&w, false) == NETWORK_NEVER, "a good link: never asked unprompted");
    check(wifi_ask_at(&w, true) == S(5), "suspected: asked at once");
    w.asked_at = S(40);
    check(wifi_ask_at(&w, true) == S(70), "suspected again: 30 s after the last ask");
    w.link = false;
    check(wifi_ask_at(&w, true) == NETWORK_NEVER, "no link: no ask, which the C6 would fail at once");
    w = joined();
    w.asked_at = S(6);
    check(wifi_ask_at(&w, false) == NETWORK_NEVER, "asked after the link: wait for the answer");
    check(!wifi_c6_silent(&w, S(22)) && wifi_c6_silent(&w, S(23)), "no answer within 17 s: the C6 is silent");
    w.internet_at = S(7);
    check(!wifi_c6_silent(&w, S(30)), "answered: not silent");
}

static void lte_checks(void) {
    lte_view l = {.state = LTE_UP, .epoch = 1, .up_at = S(30)};
    check(lte_check_at(&l, 0, 0, false, S(31)) == S(31), "after PPP up");
    l.internet = NETWORK_GOOD, l.internet_at = S(32);
    check(lte_check_at(&l, 0, S(32), false, S(100)) == S(632), "good: again 10 min after");
    check(lte_check_at(&l, 0, S(32), false, S(632)) == S(632), "10 min after a good result: checked");
    check(lte_check_at(&l, 0, S(32), true, S(152)) == S(632), "suspected within 2 min of a good result: no check");
    check(lte_check_at(&l, 0, S(32), true, S(153)) == S(153), "suspected 2 min after: checked");
    l.internet = NETWORK_UNKNOWN;
    check(lte_check_at(&l, 0, S(32), false, S(100)) == S(100), "no good result: checked");
    check(lte_check_at(&l, S(40), S(40), false, S(41)) == S(90), "failed 10 s into the link: next at 60 s into it");
    check(lte_check_at(&l, S(100), S(100), false, S(101)) == S(130), "failed later: next 30 s after");
    l.received_at = S(125);
    check(lte_check_at(&l, S(100), S(100), false, S(130)) == NETWORK_NEVER, "sessions receive: no check");
    l.state = LTE_WAITING;
    check(lte_check_at(&l, 0, 0, true, S(500)) == NETWORK_NEVER, "no PPP: no check");
    check(!lte_bearer_dead(S(30), S(89), S(120), 0), "first failure 59 s into the link: not counted");
    check(lte_bearer_dead(S(30), S(90), S(120), 0), "60 s in, 30 s apart: dead");
    check(!lte_bearer_dead(S(30), S(90), S(119), 0), "29 s apart: not yet");
    check(!lte_bearer_dead(S(30), S(90), S(120), S(95)), "received after the first failure: alive");
    check(!lte_bearer_dead(S(30), 0, S(120), 0), "no first failure");
}

static void dials(void) {
    int drops = 0, deaths = 0;
    int64_t last = -S(60);
    check(lte_dial_at(LTE_NOT_READY, 0, &drops, &deaths, 0, last, S(5)) == S(95), "not registered: 90 s");
    check(lte_dial_at(LTE_UP_ENDED, S(100), &drops, &deaths, 0, S(0), S(200)) == S(320) && drops == 1, "a link lost: 120 s");
    check(lte_dial_at(LTE_UP_ENDED, S(100), &drops, &deaths, 0, S(0), S(400)) == S(640) && drops == 2, "again: 240 s");
    drops = 3;
    check(lte_dial_at(LTE_UP_ENDED, S(100), &drops, &deaths, 0, S(0), S(400)) == S(1360) && drops == 3, "at most 960 s");
    check(lte_dial_at(LTE_UP_ENDED, S(960), &drops, &deaths, 0, S(0), S(2000)) == S(2120) && drops == 1, "960 s up forgets the doubling");
    check(lte_dial_at(LTE_ASKED, 0, &drops, &deaths, 0, S(100), S(130)) == S(160) && drops == 0, "lte on: at once, but 60 s after the last dial");
    check(lte_dial_at(LTE_ASKED, 0, &drops, &deaths, 0, S(100), S(200)) == S(200), "lte on: at once");
    check(lte_dial_at(LTE_SILENT, 0, &drops, &deaths, 0, S(0), S(100)) == S(1060), "silent modem: 960 s");
    check(lte_dial_at(LTE_REFUSED, 0, &drops, &deaths, 0, S(0), S(100)) == S(1060), "refused: 960 s");
    check(lte_dial_at(LTE_NEVER_ANSWERED, 0, &drops, &deaths, 0, S(0), S(100)) == NETWORK_NEVER, "no modem: until it speaks");
    drops = 2;
    check(lte_dial_at(LTE_BEARER_DEAD, S(200), &drops, &deaths, -S(7200), S(0), S(300)) == S(300) && deaths == 1 && drops == 2,
          "dead bearer: at once, the link doubling untouched");
    check(lte_dial_at(LTE_BEARER_DEAD, S(200), &drops, &deaths, S(300), S(300), S(600)) == S(1200) && deaths == 2, "second: 10 min");
    check(lte_dial_at(LTE_BEARER_DEAD, S(200), &drops, &deaths, S(600), S(1200), S(1500)) == S(3300) && deaths == 3, "third: 30 min");
    check(lte_dial_at(LTE_BEARER_DEAD, S(200), &drops, &deaths, S(1500), S(3300), S(3600)) == S(7200), "then 60 min");
    check(lte_dial_at(LTE_BEARER_DEAD, S(200), &drops, &deaths, S(3600), S(7200), S(3600 + 7200)) == S(3600 + 7200) && deaths == 1,
          "2 h without one forgets them");
    check(lte_dial_at(LTE_BEARER_REMADE, S(200), &drops, &deaths, S(10800), S(10800), S(10900)) == S(10900) && deaths == 1,
          "a new bearer: at once, uncounted");
}

int main(void) {
    choose();
    sessions();
    verdicts();
    hosts();
    judge();
    wifi_asks();
    lte_checks();
    dials();
    return failures != 0;
}
