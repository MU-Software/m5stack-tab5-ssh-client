// SPDX-License-Identifier: Apache-2.0
// P4_NVS's operations on this chip's default NVS partition: the C6 answers the P4 with them, the P4 its own nvs commands
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

// The answer's body into answer, at most C6_NVS_MESSAGE bytes: its length, the error in *error. Values never go to the log
size_t c6_nvs_serve(uint8_t operation, const uint8_t* body, size_t size, uint8_t* answer, esp_err_t* error);
// As c6_nvs(), from this chip's own NVS
esp_err_t c6_nvs_local(const uint8_t* request, size_t request_length, uint8_t* answer, size_t* answer_length);
