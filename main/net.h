// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

// Logs the default interface, the DNS servers, and how long resolving host and a TCP connection to port take
void net_check(const char* host, uint16_t port);
