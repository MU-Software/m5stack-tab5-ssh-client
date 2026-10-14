// SPDX-License-Identifier: GPL-3.0-or-later
#include "power.h"

#include <bsp/m5stack_tab5.h>
#include <esp_check.h>
#include <esp_log.h>

#define CHG_EN     IO_EXPANDER_PIN_NUM_7
#define NCHG_QC_EN IO_EXPANDER_PIN_NUM_5
#define CHG_STAT   IO_EXPANDER_PIN_NUM_6
#define INA226     0x41

static const char* TAG = "power";
static i2c_master_dev_handle_t monitor;

esp_err_t power_start(void) {
    // The expander at 0x44 comes out of every boot reset as inputs, which leaves both undriven
    esp_io_expander_handle_t expander = bsp_io_expander1_init();
    ESP_RETURN_ON_FALSE(expander != NULL, ESP_ERR_NOT_FOUND, TAG, "IO expander");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(expander, CHG_EN | NCHG_QC_EN, IO_EXPANDER_OUTPUT), TAG, "direction");
    return esp_io_expander_set_level(expander, CHG_EN | NCHG_QC_EN, 1);
}

static esp_err_t read16(uint8_t reg, int16_t* value) {
    uint8_t data[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(monitor, &reg, 1, data, 2, 50), TAG, "INA226");
    *value = (int16_t)(data[0] << 8 | data[1]);
    return ESP_OK;
}

void power_report(void) {
    int16_t shunt, bus;
    uint32_t stat = 0;
    if (monitor == NULL &&
        i2c_master_bus_add_device(bsp_i2c_get_handle(), &(i2c_device_config_t){.device_address = INA226, .scl_speed_hz = 100000}, &monitor) != ESP_OK)
        return;
    if (read16(0x01, &shunt) != ESP_OK || read16(0x02, &bus) != ESP_OK) return;
    esp_io_expander_get_level(bsp_io_expander1_init(), CHG_STAT, &stat);
    // Shunt 2.5 uV per bit across 5 mOhm, bus 1.25 mV per bit
    ESP_LOGI(TAG, "pack %d mV, %d mA, CHG_STAT %d", bus * 5 / 4, shunt / 2, stat != 0);
}
