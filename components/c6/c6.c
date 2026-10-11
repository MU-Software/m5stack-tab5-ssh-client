// SPDX-License-Identifier: GPL-3.0-or-later
#include "c6.h"

#include <sys/param.h>

#include <driver/i2c_master.h>
#include <eh_host_sys.h>
#include <esp_check.h>
#include <esp_hosted.h>
#include <esp_hosted_ota.h>
#include <esp_io_expander_pi4ioe5v6408.h>
#include <esp_log.h>

static const char* TAG = "c6";

// The C6's power switch is pin 0 of the second PI4IOE5V6408 on the system I2C bus (SDA 31, SCL 32)
static esp_err_t power_on(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = 31,
        .scl_io_num = 32,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    esp_io_expander_handle_t io;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c");
    ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_pi4ioe5v6408(bus, ESP_IO_EXPANDER_I2C_PI4IOE5V6408_ADDRESS_HIGH, &io), TAG, "expander");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(io, IO_EXPANDER_PIN_NUM_0, IO_EXPANDER_OUTPUT), TAG, "dir");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_output_mode(io, IO_EXPANDER_PIN_NUM_0, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL), TAG, "mode");
    return esp_io_expander_set_level(io, IO_EXPANDER_PIN_NUM_0, 1);
}

esp_err_t c6_start(void) {
    // esp_hosted retries against an unpowered C6 and starves other drivers
    ESP_RETURN_ON_ERROR(power_on(), TAG, "power");
    ESP_RETURN_ON_ERROR(esp_hosted_init(), TAG, "esp_hosted");
    return esp_hosted_connect_to_slave();
}

esp_err_t c6_update(const uint8_t* image, size_t size) {
    eh_host_coprocessor_fwver_t v = {0};
    eh_host_sys_get_cp_fw_version(&v);
    ESP_LOGI(TAG, "firmware %lu.%lu.%lu, writing %u bytes", v.major1, v.minor1, v.patch1, (unsigned)size);
    ESP_RETURN_ON_ERROR(esp_hosted_slave_ota_begin(), TAG, "begin");
    for (size_t off = 0; off < size; off += 1500) {
        esp_err_t err = esp_hosted_slave_ota_write(image + off, MIN(size - off, 1500));
        if (err != ESP_OK) {
            esp_hosted_slave_ota_end();
            return err;
        }
    }
    ESP_RETURN_ON_ERROR(esp_hosted_slave_ota_end(), TAG, "end");
    // Activation only works in the session that wrote the image; esp_hosted 1.x refuses it and boots the image on reset
    esp_err_t err = esp_hosted_slave_ota_activate();
    if (err != ESP_OK) ESP_LOGW(TAG, "activate: %s; the new firmware runs after the next reset", esp_err_to_name(err));
    return ESP_OK;
}
