// SPDX-License-Identifier: GPL-3.0-or-later
// Wi-Fi through the C6, which picks and joins the known networks and checks that its network reaches the internet; this chip
// takes the address of its lease (Network Split) and reports what the C6 says
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>
#include <esp_netif.h>

#include "network_rules.h"
#include "state.h"

// The Wi-Fi task: powers the C6, starts the station when on, and calls changed after each change of the view
esp_err_t wifi_start(const global_state* global, void (*changed)(void));
void wifi_view_now(wifi_view* out);
esp_netif_t* wifi_netif(void);
// For this boot: the view at once, then the C6, with the known networks saying so
void wifi_turn(bool on);
// After the known networks or their saved off state changed
void wifi_list_changed(void);
// The C6 leaves its network for this one once joined to it (C6_JOIN_SINCE); waits for the lease
esp_err_t wifi_join(const wifi_network* network);
// Something failed over Wi-Fi: the C6 checks its network, at most every 30 s. When its answer is due, 0 for no check
int64_t wifi_suspect(void);
// For tests: the C6 drops its network and joins it again
void wifi_drop(void);

typedef enum { WIFI_SCAN_NONE, WIFI_SCAN_RUNNING, WIFI_SCAN_DONE, WIFI_SCAN_FAILED } wifi_scan_state;
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open;
} wifi_visible_network;
// The networks in reach, once the C6 is not rejoining
void wifi_scan(void);
wifi_scan_state wifi_scan_now(void);
// Network index of the last scan, strongest first; false past the last
bool wifi_visible(size_t index, wifi_visible_network* out);
