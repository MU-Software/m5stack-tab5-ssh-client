// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Addresses as lwIP keeps them, 0 for none
typedef struct {
    uint32_t address, netmask, gateway, dns;
} c6_lease;

typedef struct {
    // The interface holds the lease's address
    bool held;
    // Changed under the same address; esp_hosted leaves them
    bool set_netmask_gateway, set_dns;
    // An IP_EVENT_STA_GOT_IP, for the DNS server to reach lwIP
    bool announce;
} c6_lease_step;

// interface NULL while not up; held as the last step left it; announced: called for an IP_EVENT_STA_GOT_IP, which needs no other
c6_lease_step c6_lease_next(const c6_lease* lease, const c6_lease* interface, bool held, bool announced);
