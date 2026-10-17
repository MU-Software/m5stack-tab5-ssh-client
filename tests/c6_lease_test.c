// SPDX-License-Identifier: GPL-3.0-or-later
#include "c6_lease.h"

#include <stdio.h>

static int failures;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

static int is(c6_lease_step s, bool held, bool set_netmask_gateway, bool set_dns, bool announce) {
    return s.held == held && s.set_netmask_gateway == set_netmask_gateway && s.set_dns == set_dns && s.announce == announce;
}

int main(void) {
    const c6_lease home = {.address = 0x2c00a8c0, .netmask = 0x00ffffff, .gateway = 0x0100a8c0, .dns = 0x0100a8c0};
    const c6_lease none = {0}, empty = {0};
    c6_lease other = home, router = home, resolver = home, plain = home, before = home;
    other.address = 0x2d00a8c0;
    router.gateway = 0xfe00a8c0;
    resolver.dns = 0x08080808;
    plain.dns = 0;
    before.address = 0x0a00000a;

    check(is(c6_lease_next(&none, &home, false, false), false, false, false, false), "no lease: nothing held");
    check(is(c6_lease_next(&home, NULL, false, false), false, false, false, false), "an interface not up yet waits for esp_hosted");
    check(is(c6_lease_next(&home, &empty, false, false), false, false, false, false), "a first address is esp_hosted's to apply");
    check(is(c6_lease_next(&other, &home, true, false), false, false, false, false), "so is a new one");
    check(is(c6_lease_next(&home, &home, false, true), true, false, false, false), "its GOT_IP makes it held, with no other");
    check(is(c6_lease_next(&home, &home, false, false), true, false, false, true), "the same lease after a rejoin announces itself");
    check(is(c6_lease_next(&home, &home, true, false), true, false, false, false), "a replay of the lease held changes nothing");
    check(is(c6_lease_next(&none, &home, true, false), false, false, false, false), "a lease gone is no longer held");
    check(is(c6_lease_next(&router, &home, true, false), true, true, false, false), "a new gateway is set, and set_ip_info announces it");
    check(is(c6_lease_next(&resolver, &home, true, false), true, false, true, true), "a new DNS server is set and announced");
    check(is(c6_lease_next(&plain, &home, true, false), true, false, false, false), "a lease without DNS leaves the server");
    check(is(c6_lease_next(&home, &before, false, false), false, false, false, false), "an address from before waits for esp_hosted");
    return failures != 0;
}
