// SPDX-License-Identifier: GPL-3.0-or-later
#include "power.h"

#include <bsp/m5stack_tab5.h>
#include <esp_check.h>

#define CHG_EN     IO_EXPANDER_PIN_NUM_7
#define NCHG_QC_EN IO_EXPANDER_PIN_NUM_5

static const char* TAG = "power";

esp_err_t power_start(void) {
    // The expander at 0x44 comes out of every boot reset as inputs, which leaves both undriven
    esp_io_expander_handle_t expander = bsp_io_expander1_init();
    ESP_RETURN_ON_FALSE(expander != NULL, ESP_ERR_NOT_FOUND, TAG, "IO expander");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(expander, CHG_EN | NCHG_QC_EN, IO_EXPANDER_OUTPUT), TAG, "direction");
    return esp_io_expander_set_level(expander, CHG_EN | NCHG_QC_EN, 1);
}
