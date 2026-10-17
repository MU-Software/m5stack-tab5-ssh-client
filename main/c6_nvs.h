// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>

// The c6 nvs commands: values go to write alone, never to a log
typedef struct {
    // c6_protocol()
    unsigned protocol;
    // c6_nvs(): 0, or an esp_err_t
    int (*exchange)(const uint8_t* request, size_t request_length, uint8_t* answer, size_t* answer_length);
    const char* (*error_name)(int error);
    void (*write)(void* context, const char* text, size_t length);
    void* context;
} c6_nvs_io;

// What follows "c6 nvs"; the last line is "c6 nvs " and ESP_OK or what failed. 0 for ESP_OK
int c6_nvs_command(const char* arguments, const c6_nvs_io* io);
