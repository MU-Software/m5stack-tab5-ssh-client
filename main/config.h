// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cJSON.h>

#include "state.h"

typedef enum { HOST_UNKNOWN, HOST_KNOWN, HOST_CHANGED } host_state;

// The configuration's network.wifi.access_points, one allocation: free(*wifi)
int config_wifi_networks(const cJSON* config, wifi_network** wifi);
// The configuration's Wi-Fi list with network ssid replaced by network (NULL removes it) where it was, first when it was not
// there, then moved by `move` places (negative: nearer the front); other networks named as network go
int config_set_wifi(cJSON* config, const char* ssid, const wifi_network* network, int move);
// An SSID of 1-32 bytes; no password, 8-63 characters or 64 hex digits
bool config_wifi_valid(const wifi_network* network);

// One allocation: free(hosts)
ssh_host* config_ssh_hosts(const char* text);

host_state known_hosts_check(const char* text, const char* name, const uint8_t* key, size_t key_length);
// key in SSH wire form; a line with options before the key type never matches, as the options would go unenforced
bool authorized_keys_check(const char* text, const uint8_t* key, size_t key_length);
int known_hosts_line(char* out, size_t size, const char* name, const uint8_t* key, size_t key_length);
// "TYPE BASE64\n", as a .pub file holds it
int public_key_line(char* out, size_t size, const uint8_t* key, size_t key_length);
int ssh_fingerprint(char* out, size_t size, const uint8_t* key, size_t key_length);

int openssh_public_key(const uint8_t* container, size_t size, const uint8_t** key, size_t* key_length);
