// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <esp_err.h>

#include "state.h"

esp_err_t wifi_connect(const wifi_network* networks, int timeout_ms);
