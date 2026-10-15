// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <cJSON.h>

#include "state.h"

typedef enum { HOST_UNKNOWN, HOST_KNOWN, HOST_CHANGED } host_state;

// One allocation: free(*wifi)
int config_wifi_networks(const cJSON* config, wifi_network** wifi);

// The log settings and the time zone; absent ones keep what they hold
int config_log(const cJSON* config, log_settings* log, char* timezone, size_t size);

// One allocation: free(hosts)
ssh_host* config_ssh_hosts(const char* text);

host_state known_hosts_check(const char* text, const char* name, const uint8_t* key, size_t key_length);
int known_hosts_line(char* out, size_t size, const char* name, const uint8_t* key, size_t key_length);
int ssh_fingerprint(char* out, size_t size, const uint8_t* key, size_t key_length);

int openssh_public_key(const uint8_t* container, size_t size, const uint8_t** key, size_t* key_length);
