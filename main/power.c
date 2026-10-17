// SPDX-License-Identifier: GPL-3.0-or-later
#include "power.h"

#include <bsp/m5stack_tab5.h>
#include <esp_check.h>
#include <esp_log.h>

// The PI4IOE5V6408 at 0x44: the BSP's driver resets it whenever it is created, which also cuts the C6's power
#define EXPANDER   0x44
#define DIRECTION  0x03
#define OUTPUT     0x05
#define HIGH_Z     0x07
#define INPUT      0x0f
#define C6_POWER   0x01
#define USB_A_5V   0x08
#define NCHG_QC_EN 0x20
#define USB_C_IN   0x40
#define CHG_EN     0x80
#define INA226     0x41

static const char* TAG = "power";
static i2c_master_dev_handle_t expander, monitor;

static esp_err_t update(uint8_t reg, uint8_t mask, uint8_t bits) {
    uint8_t value;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(expander, &reg, 1, &value, 1, 50), TAG, "read %02x", reg);
    return i2c_master_transmit(expander, (uint8_t[]){reg, (uint8_t)((value & ~mask) | bits)}, 2, 50);
}

esp_err_t power_start(void) {
    // From power-on every pin is a high-impedance input pulled down: charging off, the C6 and USB-A off. A restart keeps them.
    static const uint8_t pins = C6_POWER | USB_A_5V | NCHG_QC_EN | CHG_EN;
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C");
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(bsp_i2c_get_handle(), &(i2c_device_config_t){.device_address = EXPANDER, .scl_speed_hz = 100000}, &expander), TAG,
        "IO expander");
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &(i2c_device_config_t){.device_address = INA226, .scl_speed_hz = 100000}, &monitor) != ESP_OK)
        ESP_LOGW(TAG, "no INA226");
    // NCHG_QC_EN high adds a resistor on the IP2326's NTC pin that reads as a warm battery, which halves the charge current
    ESP_RETURN_ON_ERROR(update(OUTPUT, pins, C6_POWER | USB_A_5V | CHG_EN), TAG, "levels");
    // USB_C_IN back to an input, which the BSP's driver made an output that reads 0
    ESP_RETURN_ON_ERROR(update(DIRECTION, pins | USB_C_IN, pins), TAG, "directions");
    return update(HIGH_Z, pins, 0);
}

static esp_err_t read16(uint8_t reg, int16_t* value) {
    uint8_t data[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(monitor, &reg, 1, data, 2, 50), TAG, "INA226");
    *value = (int16_t)(data[0] << 8 | data[1]);
    return ESP_OK;
}

void power_report(void) {
    int16_t shunt, bus;
    uint8_t reg = INPUT, stat = 0;
    if (monitor == NULL || read16(0x01, &shunt) != ESP_OK || read16(0x02, &bus) != ESP_OK) return;
    if (expander != NULL) i2c_master_transmit_receive(expander, &reg, 1, &stat, 1, 50);
    // Shunt 2.5 uV per bit across 5 mOhm, bus 1.25 mV per bit; charging past the monitor's noise of a few mA
    int milliamps = shunt / 2;
    ESP_LOGI(TAG, "pack %d mV, %d mA%s, USB-C %s", bus * 5 / 4, milliamps, milliamps < -10 ? " charging" : "", stat & USB_C_IN ? "in" : "out");
}

bool power_battery(int* millivolts) {
    int16_t bus;
    if (monitor == NULL || read16(0x02, &bus) != ESP_OK) return false;
    *millivolts = bus * 5 / 4;
    return true;
}
