// SPDX-License-Identifier: Apache-2.0
#include <esp_event.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <nvs_flash.h>

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // The C6 can only be reflashed through the back-cover pads: keep an image only once it has stayed up
    vTaskDelay(pdMS_TO_TICKS(30000));
    esp_ota_mark_app_valid_cancel_rollback();
}
