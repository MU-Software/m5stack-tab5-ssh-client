// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>

#include <esp_err.h>
#include <esp_netif.h>
#include <esp_wifi_types.h>

#include "state.h"

// Powers the C6 and starts the station when on, without joining; changed is called from esp_hosted's task after each Wi-Fi
// state the C6 reports
esp_err_t wifi_start(bool on, void (*changed)(void));
// The networks in reach, freed by the caller; ends the retries of a joined network that has gone
esp_err_t wifi_scan(wifi_ap_record_t** records, uint16_t* count);
// Joins the first of networks but except that a scan sees, leaving any network joined before
esp_err_t wifi_join(const wifi_network* networks, const char* except, int timeout_ms);
esp_err_t wifi_on(void);
esp_err_t wifi_off(void);
// Joined with an address; a lost network is retried until wifi_join or wifi_off
bool wifi_connected(void);
esp_netif_t* wifi_netif(void);
