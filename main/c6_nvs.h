// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>

// The nvs and c6 nvs commands: values go to write alone, never to a log
typedef struct {
    // "nvs" or "c6 nvs", which starts the last line
    const char* name;
    // c6_protocol(), or C6_PROTOCOL for this chip's own
    unsigned protocol;
    // c6_nvs() or c6_nvs_local(): 0, or an esp_err_t
    int (*exchange)(const uint8_t* request, size_t request_length, uint8_t* answer, size_t* answer_length);
    const char* (*error_name)(int error);
    void (*write)(void* context, const char* text, size_t length);
    // load's input: bytes, 0 at its end, negative when it broke off; NULL where there is none
    int (*read)(void* context, void* buffer, size_t length);
    void* context;
} c6_nvs_io;

// What follows the name; the last line is the name and ESP_OK or what failed. 0 for ESP_OK
int c6_nvs_command(const char* arguments, const c6_nvs_io* io);
