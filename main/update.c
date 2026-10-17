// SPDX-License-Identifier: GPL-3.0-or-later
#include "update.h"

#include <stdatomic.h>

#include <esp_attr.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "c6.h"

#define OVER_NETWORK 0x4e455457u

static const char* TAG = "update";
// Survives esp_restart(): the path a new image came over, which must work again for it to be kept. A network update
// with USB plugged in still waits for the network, the path it has to keep working.
static __NOINIT_ATTR uint32_t installed_over;
static update_path proof;
static esp_timer_handle_t rollback;

static void roll_back(void* arg) {
    (void)arg;
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

esp_err_t update_start(void) {
    proof = installed_over == OVER_NETWORK ? UPDATE_NETWORK : UPDATE_USB;
    installed_over = 0;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) return ESP_OK;
    ESP_RETURN_ON_ERROR(esp_timer_create(&(esp_timer_create_args_t){.callback = roll_back, .name = "rollback"}, &rollback), TAG, "timer");
    // Wi-Fi and a login from afar take longer than USB enumerating
    return esp_timer_start_once(rollback, (proof == UPDATE_NETWORK ? 60 : 30) * 1000000LL);
}

esp_err_t update_install(bool c6, const uint8_t* image, size_t size) {
    if (c6) return c6_update(image, size);
    const esp_partition_t* slot = esp_ota_get_next_update_partition(NULL);
    if (slot == NULL || size > slot->size) return ESP_ERR_INVALID_SIZE;
    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(slot, size, &ota);
    if (err == ESP_OK) err = esp_ota_write(ota, image, size);
    if (ota != 0) err = err == ESP_OK ? esp_ota_end(ota) : (esp_ota_abort(ota), err);
    return err == ESP_OK ? esp_ota_set_boot_partition(slot) : err;
}

void update_restart(update_path path) {
    installed_over = path == UPDATE_NETWORK ? OVER_NETWORK : 0;
    vTaskDelay(pdMS_TO_TICKS(200)); // for the host to read the reply
    esp_restart();
}

static void keep(int check) {
    static atomic_int passed;
    if ((atomic_fetch_or(&passed, check) | check) == 3 && rollback != NULL && esp_timer_stop(rollback) == ESP_OK) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new image kept");
    }
}

void update_works(update_path path) {
    if (path == proof) keep(1);
}

void update_ran(void) {
    keep(2);
}
