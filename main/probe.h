// SPDX-License-Identifier: GPL-3.0-or-later
// Bound to one path: none leaves by another, even with Wi-Fi down. Each holds the Tab5 awake; a light sleep inside gives EINTR or PROBE_SLEPT
#pragma once
#include <stdint.h>

#include <esp_netif.h>

// Looked up afresh: wifi off and on renames it. ENODEV once gone
int probe_bind(int fd, esp_netif_t* netif);
// 0 or an errno: ECONNREFUSED for a refusal, ENETDOWN at once without an address
int probe_connect(esp_netif_t* netif, uint32_t address, uint16_t port, int timeout_ms);
typedef enum { PROBE_FOUND, PROBE_NO_NAME, PROBE_NO_ANSWER, PROBE_DOWN, PROBE_SLEPT } probe_lookup;
// try_ms per server: the path's, then 1.1.1.1; a name the path's servers deny still goes to 1.1.1.1
probe_lookup probe_resolve(esp_netif_t* netif, const char* name, int try_ms, uint32_t* address);
// For tests: netif's DNS servers never answer; NULL for none
void probe_dead_dns(esp_netif_t* netif);
