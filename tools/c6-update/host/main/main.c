// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>

#include <esp_log.h>

#include "c6.h"

extern const uint8_t _binary_tab5_c6_bin_start[], _binary_tab5_c6_bin_end[]; // NOLINT(bugprone-reserved-identifier)

void app_main(void) {
    ESP_ERROR_CHECK(c6_start());
    ESP_ERROR_CHECK(c6_update(_binary_tab5_c6_bin_start, (uintptr_t)_binary_tab5_c6_bin_end - (uintptr_t)_binary_tab5_c6_bin_start));
    ESP_LOGI("c6-update", "done; flash the app again");
}
