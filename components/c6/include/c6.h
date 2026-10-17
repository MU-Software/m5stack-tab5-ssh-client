// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

esp_err_t c6_start(void);
// The C6's esp_hosted version; false when unknown or of another major version than the host's
bool c6_version(char* version, size_t size);
esp_err_t c6_update(const uint8_t* image, size_t size);
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
// Called from esp_hosted's task after each C6_WIFI_STATE
void c6_on_wifi_state(void (*listener)(void));
