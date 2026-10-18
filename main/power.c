// SPDX-License-Identifier: GPL-3.0-or-later
#include "power.h"

#include <bsp/m5stack_tab5.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>

// The PI4IOE5V6408 at 0x44: the BSP's driver resets it whenever it is created, which also cuts the C6's power
#define EXPANDER   0x44
#define DIRECTION  0x03
#define OUTPUT     0x05
#define HIGH_Z     0x07
#define INPUT      0x0f
#define C6_POWER   0x01
#define USB_A_5V   0x08
#define NCHG_QC_EN 0x20
#define CHG_LED    0x40
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
    // CHG_LED back to an input, which the BSP's driver made an output that reads 0
    ESP_RETURN_ON_ERROR(update(DIRECTION, pins | CHG_LED, pins), TAG, "directions");
    return update(HIGH_Z, pins, 0);
}

esp_err_t power_usb_a(bool on) {
    return expander == NULL ? ESP_ERR_INVALID_STATE : update(OUTPUT, USB_A_5V, on ? USB_A_5V : 0);
}

void power_average(bool on) {
    // 1024 samples of 4.156 ms per channel, else the power-on default: 1 sample of 1.1 ms
    uint16_t config = on ? 0x4fb7 : 0x4127;
    if (monitor != NULL) i2c_master_transmit(monitor, (uint8_t[]){0x00, (uint8_t)(config >> 8), (uint8_t)config}, 3, 50);
}

static esp_err_t read16(uint8_t reg, int16_t* value) {
    uint8_t data[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(monitor, &reg, 1, data, 2, 50), TAG, "INA226");
    *value = (int16_t)(data[0] << 8 | data[1]);
    return ESP_OK;
}

// Shunt 2.5 uV per bit across 5 mOhm. Charging is current into the pack past the monitor's noise of a few mA, as USB-C or the DC
// input powers the board instead of the pack; CHG_LED, the IP2326's LED line, reads 1 on battery too.
static bool charging_at(int16_t shunt) {
    return shunt / 2 < -10;
}

void power_report(void) {
    int16_t shunt, bus;
    uint8_t reg = INPUT, stat = 0;
    if (monitor == NULL || read16(0x01, &shunt) != ESP_OK || read16(0x02, &bus) != ESP_OK) return;
    if (expander != NULL) i2c_master_transmit_receive(expander, &reg, 1, &stat, 1, 50);
    // Bus 1.25 mV per bit
    ESP_LOGI(TAG, "pack %d mV, %d mA, %s, LED %d", bus * 5 / 4, shunt / 2, charging_at(shunt) ? "charging" : "not charging", (stat & CHG_LED) != 0);
}

void power_pack(pack_reading* out) {
    static int64_t missing_at;
    int16_t shunt, bus;
    uint8_t reg = INPUT, stat = 0;
    out->known = monitor != NULL && read16(0x01, &shunt) == ESP_OK && read16(0x02, &bus) == ESP_OK;
    if (!out->known) return;
    if (expander != NULL) i2c_master_transmit_receive(expander, &reg, 1, &stat, 1, 50);
    int64_t now = esp_timer_get_time();
    out->millivolts = bus * 5 / 4;
    out->flow = charging_at(shunt) ? PACK_CHARGING : shunt / 2 >= 30 ? PACK_DISCHARGING : PACK_IDLE;
    // No pack: the charger's output alone, which falls to ~4.4 V for ~5 s every ~16 s while CHG_LED blinks; a full pack keeps CHG_LED
    // off (measured 10-19). Held 25 s, as it is read every 5 s.
    if (out->flow != PACK_IDLE)
        missing_at = 0;
    else if (out->millivolts < 6000 || (stat & CHG_LED) != 0)
        missing_at = now;
    if (out->flow == PACK_IDLE && missing_at != 0 && now - missing_at < 25 * 1000000) out->flow = PACK_MISSING;
}
