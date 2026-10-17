// SPDX-License-Identifier: GPL-3.0-or-later
// Network Split: the C6 runs DHCP, and this chip's Wi-Fi interface takes the address of its lease
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
    // Changed under the same address, which esp_hosted leaves as they were
    bool set_netmask_gateway, set_dns;
    // An IP_EVENT_STA_GOT_IP, for the DNS servers to reach lwIP: the address came back or the DNS server changed
    bool announce;
} c6_lease_step;

// The C6's last lease against what the interface holds (NULL while it is not up), held as the step before left it; announced
// for an IP_EVENT_STA_GOT_IP, which needs no other
c6_lease_step c6_lease_next(const c6_lease* lease, const c6_lease* interface, bool held, bool announced);
