// SPDX-License-Identifier: GPL-3.0-or-later
#include "c6_lease.h"

#include <stddef.h>

c6_lease_step c6_lease_next(const c6_lease* lease, const c6_lease* interface, bool held, bool announced) {
    c6_lease_step step = {0};
    // A new address is esp_hosted's to apply, with the STA_CONNECTED it makes up and the frames it then lets in; its GOT_IP
    // comes back here
    if (lease->address == 0 || interface == NULL || interface->address != lease->address) return step;
    step.held = true;
    step.set_netmask_gateway = interface->netmask != lease->netmask || interface->gateway != lease->gateway;
    step.set_dns = lease->dns != 0 && interface->dns != lease->dns;
    // esp_netif_set_ip_info posts its own
    step.announce = !step.set_netmask_gateway && (step.set_dns || (!held && !announced));
    return step;
}
