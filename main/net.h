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
// Blocks until there is a path; its generation
unsigned net_wait(void);
// After a dropped session: checks Wi-Fi again before the next connection, which may then go over LTE
void net_suspect(void);
// The path's address, for binding a connection to it
uint32_t net_local_address(void);
// TCP keepalive for the path
void net_keepalive(int fd);
// Joins network now
esp_err_t net_join(const wifi_network* network);
// wifi on|off, lte on|off|status, net status, net check HOST PORT
bool net_command(const char* line);
// Logs the default interface, the DNS servers, and how long resolving host and a TCP connection to port take
void net_check(const char* host, uint16_t port);
