// SPDX-License-Identifier: Apache-2.0
#include "c6_pick.h"

#include <stdio.h>

static int failures;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

int main(void) {
    const int8_t rssi[4] = {-60, -40, -90, INT8_MIN};
    const int64_t none[4] = {0}, backoff[4] = {200, 0, 0, 0}, demoted[4] = {500, 0, 0, 0};
    check(c6_next_network(rssi, none, none, 4, 0, 100) == 0, "the first in list order, not the strongest");
    check(c6_next_network(rssi, backoff, none, 4, 0, 100) == 1, "one backing off is passed over");
    check(c6_next_network(rssi, backoff, none, 4, 0, 200) == 0, "until its backoff ends");
    check(c6_next_network(rssi, none, none, 4, 1u << 0, 100) == 1, "one tried is skipped");
    check(c6_next_network(rssi, none, none, 4, 1u << 0 | 1u << 1, 100) == -1, "below the floor or out of reach: none");
    check(c6_next_network(rssi, none, demoted, 4, 0, 100) == 1, "one that did not reach the host goes after the others");
    check(c6_next_network(rssi, none, demoted, 4, 1u << 1, 100) == 0, "but is still tried when no other is");
    check(c6_next_network(rssi, none, demoted, 4, 0, 500) == 0, "and is first again once its demotion ends");
    return failures != 0;
}
