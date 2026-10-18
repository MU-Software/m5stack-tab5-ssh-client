// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#include "c6_lease.h"

esp_err_t c6_start(void);
// The C6's esp_hosted version; false when unknown or of another major version than the host's
bool c6_version(char* version, size_t size);
esp_err_t c6_update(const uint8_t* image, size_t size);
// esp_hosted's SDIO transfers wait from the end of the one under way, if it ends within milliseconds, until released
bool c6_hold_transfers(uint32_t milliseconds);
void c6_release_transfers(void);
// The version of c6_protocol.h the C6 firmware speaks, 0 for one without it
unsigned c6_protocol(void);
// A P4_TO_C6 message, its kind first
esp_err_t c6_send(const uint8_t* message, size_t size);
// A P4_NVS request's body and the C6_NVS answer's into answer, at most *answer_length bytes: the error of the exchange, else the
// C6's; ESP_ERR_NOT_SUPPORTED from a C6 firmware older than C6_NVS_SINCE
esp_err_t c6_nvs(const uint8_t* request, size_t request_length, uint8_t* answer, size_t* answer_length);
typedef struct {
    // C6_WIFI_* of c6_protocol.h
    uint8_t state, reason;
    int8_t rssi;
    char ssid[33];
} c6_wifi_status;
// The last C6_WIFI_STATE into status when it is not NULL; how many came this boot
unsigned c6_wifi_state(c6_wifi_status* status);
// Called from the default event loop after each C6_WIFI_STATE
void c6_on_wifi_state(void (*listener)(void));
typedef struct {
    // errno of the last address tried, 0 once one answered
    int error;
    unsigned milliseconds;
    char address[16];
} c6_reach_answer;
// From C6_INTERNET_SINCE: the C6 checks its network now; its answer comes to c6_on_internet's listener with sequence (1-255)
esp_err_t c6_ask_internet(uint8_t sequence);
// Called from the default event loop with each C6_INTERNET: answers to c6_ask_internet, and the C6's own checks with sequence 0
void c6_on_internet(void (*listener)(uint8_t sequence, const c6_reach_answer* answer));
// From C6_JOIN_SINCE: the C6 leaves its network for this one. ESP_OK once joined, ESP_ERR_NOT_FOUND when it saw none,
// ESP_ERR_INVALID_STATE with its Wi-Fi stopped, else ESP_FAIL with the reason of the failure in *reason
esp_err_t c6_join(const char* ssid, const char* password, int timeout_ms, uint8_t* reason);
// From C6_NW_SPLIT_SINCE: called from the default event loop with each lease the C6 reports, address 0 when it has none
void c6_on_lease(void (*listener)(const c6_lease* lease));
// The C6's lease now, through esp_hosted's own handler and the listener
esp_err_t c6_ask_lease(void);
