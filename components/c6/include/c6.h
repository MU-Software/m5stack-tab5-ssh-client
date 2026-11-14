// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#include "c6_lease.h"

esp_err_t c6_start(void);
// esp_hosted's; false when unknown or of another major version than the host's
bool c6_version(char* version, size_t size);
esp_err_t c6_update(const uint8_t* image, size_t size);
// Holds esp_hosted's SDIO transfers until released; false when the one under way outlasts milliseconds
bool c6_hold_transfers(uint32_t milliseconds);
void c6_release_transfers(void);
// The C6 firmware's C6_PROTOCOL, 0 for one without c6_protocol.h
unsigned c6_protocol(void);
// A P4_* message, kind first
esp_err_t c6_send(const uint8_t* message, size_t size);
// P4_NVS's [operation][body] in, C6_NVS's body out, at most *answer_length bytes: the exchange's error, else the C6's;
// ESP_ERR_NOT_SUPPORTED before C6_NVS_SINCE
esp_err_t c6_nvs(const uint8_t* request, size_t request_length, uint8_t* answer, size_t* answer_length);
typedef struct {
    // state: C6_WIFI_*
    uint8_t state, reason;
    int8_t rssi;
    char ssid[33];
} c6_wifi_status;
// The last C6_WIFI_STATE into status when not NULL; how many came this boot
unsigned c6_wifi_state(c6_wifi_status* status);
// Called from the default event loop after each C6_WIFI_STATE
void c6_on_wifi_state(void (*listener)(void));
typedef struct {
    // errno of the last address tried, 0 once one answered
    int error;
    unsigned milliseconds;
    char address[16];
} c6_reach_answer;
// From C6_INTERNET_SINCE; sequence 1-255, answered to c6_on_internet's listener
esp_err_t c6_ask_internet(uint8_t sequence);
// Called from the default event loop with each C6_INTERNET; sequence 0 for the C6's own checks
void c6_on_internet(void (*listener)(uint8_t sequence, const c6_reach_answer* answer));
// From C6_JOIN_SINCE. ESP_ERR_NOT_FOUND: none seen, ESP_ERR_INVALID_STATE: its Wi-Fi stopped, else ESP_FAIL with the reason in *reason
esp_err_t c6_join(const char* ssid, const char* password, int timeout_ms, uint8_t* reason);
// From C6_NW_SPLIT_SINCE: called from the default event loop with each lease, address 0 for none
void c6_on_lease(void (*listener)(const c6_lease* lease));
// The lease now, to esp_hosted's own handler and the listener
esp_err_t c6_ask_lease(void);
