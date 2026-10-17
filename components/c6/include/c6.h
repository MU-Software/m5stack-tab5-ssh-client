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
