// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

esp_err_t c6_start(void);
esp_err_t c6_update(const uint8_t* image, size_t size);
