// SPDX-License-Identifier: GPL-3.0-or-later
#include "c6.h"

#include <sys/param.h>

#include <bsp/m5stack_tab5.h>
#include <eh_host_sys.h>
#include <esp_check.h>
#include <esp_hosted.h>
#include <esp_hosted_ota.h>
#include <esp_log.h>

static const char* TAG = "c6";

esp_err_t c6_start(void) {
    // esp_hosted retries against an unpowered C6 and starves other drivers
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_WIFI, true), TAG, "power");
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
