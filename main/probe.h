// SPDX-License-Identifier: GPL-3.0-or-later
// Sockets bound to one path's interface (SO_BINDTODEVICE): nothing leaves by another path, also while Wi-Fi is down. Each holds
// the Tab5 awake, and a light sleep inside one makes it EINTR or PROBE_SLEPT
#pragma once
#include <stdint.h>

#include <esp_netif.h>

// fd bound to netif's lwIP interface, looked up afresh (wifi off and on renames it): 0, or ENODEV once it is gone
int probe_bind(int fd, esp_netif_t* netif);
// A TCP connection to address:port through netif: 0 or an errno, ECONNREFUSED for an answer that refused; ENETDOWN at once
// without an address
int probe_connect(esp_netif_t* netif, uint32_t address, uint16_t port, int timeout_ms);
typedef enum { PROBE_FOUND, PROBE_NO_NAME, PROBE_NO_ANSWER, PROBE_DOWN, PROBE_SLEPT } probe_lookup;
// name's address: an IP as it is, else from the path's DNS servers, then 1.1.1.1, all through netif and try_ms each, an answer
// from one asked earlier counting meanwhile; a name the path's servers deny is asked of 1.1.1.1 once more
probe_lookup probe_resolve(esp_netif_t* netif, const char* name, int try_ms, uint32_t* address);
// TCP port 443 of the public addresses in turn through netif, any answer counting: 0, else the last errno; address the last tried
int probe_internet(esp_netif_t* netif, int per_address_ms, char address[16]);
// For tests: netif's DNS servers seen as 192.0.2.53, which never answers; NULL for none
void probe_dead_dns(esp_netif_t* netif);
