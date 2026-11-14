// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#include "network_rules.h"
#include "state.h"

// Starts Wi-Fi and LTE too
esp_err_t network_start(const global_state* global);
// For a path
bool network_wait_for(int timeout_ms);

typedef struct {
    network_use use;
    uint32_t address;
} network_route;
// Blocks the caller, also through a Wi-Fi rejoin; route filled for NETWORK_FINE
network_trouble network_find(const char* host, network_route* route);
bool network_moved(const network_use* use);
// ran: the session had connected. Blocks for the path's check; NETWORK_FINE when the host answers or the session should move
network_trouble network_after_drop(const ssh_host* host, const network_route* used, bool ran);
// An ssh_session's prepare; route: a network_route
int network_bind(int fd, void* route);
void network_keepalive(int fd, network_path path);
// Per connection step: CAT-M is slow
int network_timeout_ms(network_path path);

// Asks the C6 for the access point's signal
void network_describe(network_info* out);
bool network_command(const char* line);
