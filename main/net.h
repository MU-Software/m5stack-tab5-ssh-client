// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
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
// After the known networks or their saved off state changed
void net_wifi_changed(void);
typedef enum { NET_SCAN_NONE, NET_SCAN_RUNNING, NET_SCAN_DONE, NET_SCAN_FAILED } net_scan_state;
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open;
} net_visible_network;
// Asks for a scan of the Wi-Fi networks in reach
void net_scan(void);
net_scan_state net_scan_state_now(void);
// Network `index` of the last scan, strongest first; false past the last
bool net_visible(size_t index, net_visible_network* out);
typedef struct {
    // "none", "Wi-Fi" or "LTE"
    const char* path;
    bool wifi_on, wifi_joined, wifi_reaches, lte_on;
    net_lte_state lte;
    char ssid[33], wifi_address[16], lte_address[16];
    int8_t rssi;
} net_info;

// What the screens show of the network; asks the C6 for the access point
void net_describe(net_info* out);
// wifi on|off, lte on|off|status, net status, net check HOST PORT
bool net_command(const char* line);
// Logs the default interface, the DNS servers, and how long resolving host and a TCP connection to port take
void net_check(const char* host, uint16_t port);
