// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#include "state.h"

// Wi-Fi and LTE together; the path is Wi-Fi while it reaches the SSH host, else LTE
esp_err_t net_start(const global_state* state);
// Bumped whenever the path changes
unsigned net_generation(void);
// Waits up to timeout_ms for a path; its generation
bool net_wait_for(int timeout_ms, unsigned* path);
typedef enum { NET_LTE_OFF, NET_LTE_NO_MODEM, NET_LTE_DIALING, NET_LTE_REGISTERING, NET_LTE_WAITING, NET_LTE_UP } net_lte_state;
// What LTE is doing, for a screen without a path
net_lte_state net_lte(void);
// After a dropped session: checks Wi-Fi again before the next connection, which may then go over LTE
void net_suspect(void);
// The path's address, for binding a connection to it
uint32_t net_local_address(void);
// TCP keepalive for the path
void net_keepalive(int fd);
// The path is LTE, with its seconds-long stalls
bool net_on_lte(void);
// Joins network now
esp_err_t net_join(const wifi_network* network);
// wifi on|off, lte on|off|status, net status, net check HOST PORT
bool net_command(const char* line);
// Logs the default interface, the DNS servers, and how long resolving host and a TCP connection to port take
void net_check(const char* host, uint16_t port);
