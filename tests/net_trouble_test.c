// SPDX-License-Identifier: GPL-3.0-or-later
#include "net_trouble.h"

#include <stdio.h>

static int failures;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

int main(void) {
    check(net_judge(true, true, true) == NET_FINE, "connected: fine");
    check(net_judge(true, true, false) == NET_FINE, "connected, whatever the internet check said");
    check(net_judge(false, true, true) == NET_HOST_DOWN, "resolved, internet, no answer: the host is down, no network to blame");
    check(net_judge(false, false, true) == NET_NO_NAME, "internet but no name even from the public servers: DNS");
    check(net_judge(false, false, false) == NET_NO_INTERNET, "no internet: the network");
    check(net_judge(false, true, false) == NET_NO_INTERNET, "resolved from a cache but no internet: the network");
    check(net_trouble_text(NET_FINE) == NULL && net_trouble_text(NET_HOST_DOWN) != NULL, "texts for the waiting session");
    return failures != 0;
}
