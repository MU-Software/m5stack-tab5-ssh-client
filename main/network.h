// SPDX-License-Identifier: GPL-3.0-or-later
// The path: Wi-Fi while it reaches the internet, else LTE, each path checked through itself; SSH hosts are reached over the
// path chosen, or over the other when only its DNS knows the host
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#include "network_rules.h"
#include "state.h"

// Wi-Fi, LTE and the task that chooses between them
esp_err_t network_start(const global_state* global);
// Waits up to timeout_ms for a path
bool network_wait_for(int timeout_ms);

// A host's path and address
typedef struct {
    network_use use;
    uint32_t address;
} network_route;
// NETWORK_FINE with route filled, else why not. A Wi-Fi rejoining is waited for. Blocks the calling task only
network_trouble network_find(const char* host, network_route* route);
// The path a session uses changed, renewed or went: it connects again
bool network_moved(const network_use* use);
// After a session dropped (ran) or did not connect: why its host was not reached over the route it used, once that path is
// checked again; NETWORK_FINE when the host answers or the session should move
network_trouble network_after_drop(const ssh_host* host, const network_route* used, bool ran);
// A socket about to connect over route's path: bound to its interface (an ssh_session's prepare)
int network_bind(int fd, void* route);
// TCP keepalive for path
void network_keepalive(int fd, network_path path);
// The connection's step timeout over path: CAT-M is slow
int network_timeout_ms(network_path path);

// What the screens show; asks the C6 for the access point's signal
void network_describe(network_info* out);
// wifi on|off|drop, lte on|off|status|probe dead on|off, modem read|at COMMAND, net status, net check HOST PORT [wifi|lte],
// net dns dead wifi|lte|off
bool network_command(const char* line);
