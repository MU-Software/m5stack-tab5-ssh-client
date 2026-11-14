// SPDX-License-Identifier: GPL-3.0-or-later
// The C6 picks, joins and checks the known networks; this chip takes its lease (Network Split)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>
#include <esp_netif.h>

#include "network_rules.h"
#include "state.h"

// Powers the C6; changed: after each view change
esp_err_t wifi_start(const global_state* global, void (*changed)(void));
void wifi_view_now(wifi_view* out);
esp_netif_t* wifi_netif(void);
// This boot only: the view at once, then the C6 through the list
void wifi_turn(bool on);
// After the known networks or their saved off state changed
void wifi_list_changed(void);
// The C6 leaves its network only once joined to this one; blocks until the lease
esp_err_t wifi_join(const wifi_network* network);
// After a failure over Wi-Fi, at most every 30 s: when the answer is due, 0 for no check
int64_t wifi_suspect(void);
// For tests: the C6 drops and rejoins
void wifi_drop(void);

typedef enum { WIFI_SCAN_NONE, WIFI_SCAN_RUNNING, WIFI_SCAN_DONE, WIFI_SCAN_FAILED } wifi_scan_state;
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open;
} wifi_visible_network;
// Once the C6 is not rejoining
void wifi_scan(void);
wifi_scan_state wifi_scan_now(void);
// The index-th of the last scan not in saved, strongest first; false past the last
bool wifi_visible(const wifi_network* saved, size_t index, wifi_visible_network* out);
