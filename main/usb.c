// SPDX-License-Identifier: GPL-3.0-or-later
#include "usb.h"

#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include <esp_attr.h>
#include <esp_check.h>
#include <esp_mac.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_rom_crc.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/usb_wrap_ll.h>
#include <tinyusb.h>
#include <tinyusb_cdc_acm.h>
#include <tinyusb_console.h>

#define STORAGE_BOOT 0x53444f4eu

static const char* TAG = "usb";
// Survives esp_restart(): the full-speed controller has 4 IN endpoints, too few for two CDC ports and mass storage at once
static __NOINIT_ATTR uint32_t boot_mode;
static int storage_boot;
static tinyusb_cdcacm_itf_t control_port;
static esp_timer_handle_t rollback;
static bool (*app_command)(const char* line);

static const uint8_t terminal_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, TUD_CONFIG_DESC_LEN + 2 * TUD_CDC_DESC_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(0, 0, 0x81, 8, 0x02, 0x82, 64),
    TUD_CDC_DESCRIPTOR(2, 0, 0x83, 8, 0x04, 0x84, 64),
};
static const uint8_t storage_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(0, 0, 0x81, 8, 0x02, 0x82, 64),
    TUD_MSC_DESCRIPTOR(2, 0, 0x03, 0x83, 64),
};

int usb_storage_boot(void) {
    return storage_boot;
}

static void put(tinyusb_cdcacm_itf_t port, const void* buffer, size_t size) {
    const uint8_t* p = buffer;
    for (int idle = 0; size > 0 && idle < 100;) {
        size_t n = tinyusb_cdcacm_write_queue(port, p, size);
        p += n;
        size -= n;
        if (tinyusb_cdcacm_write_flush(port, pdMS_TO_TICKS(10)) != ESP_OK && n == 0) idle++;
    }
}

size_t usb_terminal_read(void* buffer, size_t size) {
    size_t n = 0;
    return tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buffer, size, &n) == ESP_OK ? n : 0;
}

// Only to an open port: a closed one fills up and holds every write for a second
void usb_terminal_write(const void* buffer, size_t size) {
    if (tud_cdc_n_connected(TINYUSB_CDC_ACM_0)) put(TINYUSB_CDC_ACM_0, buffer, size);
}

uint8_t* usb_receive(size_t size, uint32_t crc, esp_err_t* err) {
    uint8_t* image = size > 0 ? heap_caps_malloc(size, MALLOC_CAP_SPIRAM) : NULL;
    *err = ESP_ERR_INVALID_SIZE;
    if (image == NULL) return NULL;
    put(control_port, "ready\n", 6);
    size_t got = 0;
    for (TickType_t last = xTaskGetTickCount(); got < size && xTaskGetTickCount() - last < pdMS_TO_TICKS(5000);) {
        size_t n = 0;
        // TinyUSB truncates a read's size to 16 bits: 65536 bytes left would read nothing, forever
        if (tinyusb_cdcacm_read(control_port, image + got, MIN(size - got, 4096), &n) == ESP_OK && n > 0) {
            got += n;
            last = xTaskGetTickCount();
        } else {
            vTaskDelay(1);
        }
    }
    *err = got != size ? ESP_ERR_TIMEOUT : esp_rom_crc32_le(0, image, size) != crc ? ESP_ERR_INVALID_CRC : ESP_OK;
    if (*err == ESP_OK) return image;
    free(image);
    return NULL;
}

void usb_reply(const char* text) {
    put(control_port, text, strlen(text));
}

static esp_err_t receive_update(size_t size, uint32_t crc) {
    const esp_partition_t* slot = esp_ota_get_next_update_partition(NULL);
    if (slot == NULL || size > slot->size) return ESP_ERR_INVALID_SIZE;
    esp_err_t err;
    uint8_t* image = usb_receive(size, crc, &err);
    if (image == NULL) return err;
    esp_ota_handle_t ota = 0;
    err = esp_ota_begin(slot, size, &ota);
    if (err == ESP_OK) err = esp_ota_write(ota, image, size);
    free(image);
    if (ota != 0) err = err == ESP_OK ? esp_ota_end(ota) : (esp_ota_abort(ota), err);
    return err == ESP_OK ? esp_ota_set_boot_partition(slot) : err;
}

static void command(char* line) {
    unsigned long size, crc;
    if (strcmp(line, "sd on") == 0) {
        boot_mode = STORAGE_BOOT;
        esp_restart();
    } else if (strcmp(line, "sd off") == 0 || strcmp(line, "restart") == 0) {
        esp_restart();
    } else if (sscanf(line, "update %lu %lx", &size, &crc) == 2) {
        esp_err_t err = receive_update(size, crc);
        char reply[48];
        int n = snprintf(reply, sizeof reply, "update %s\n", esp_err_to_name(err));
        put(control_port, reply, (size_t)n);
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(200)); // for the host to read the reply
            esp_restart();
        }
    } else if (line[0] && !app_command(line)) {
        ESP_LOGW(TAG,
                 "commands: sd on|off, restart, update SIZE CRC32, c6 update SIZE CRC32, wifi on|off [save]|connect SSID [PASSWORD]|forget SSID, lte "
                 "on|off|status, net "
                 "status|check HOST PORT, modem status|read");
    }
}

static void control(void* arg) {
    (void)arg;
    char line[48];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        size_t got = 0;
        if (tinyusb_cdcacm_read(control_port, &c, 1, &got) != ESP_OK || got == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        } else if (c == '\r' || c == '\n') {
            line[n] = 0;
            command(line);
            n = 0;
        } else if (n + 1 < sizeof line) {
            line[n++] = (char)c;
        }
    }
}

static void keep_image(int check) {
    static atomic_int passed;
    if ((atomic_fetch_or(&passed, check) | check) == 3 && rollback != NULL && esp_timer_stop(rollback) == ESP_OK)
        esp_ota_mark_app_valid_cancel_rollback();
}

void usb_keep_image(void) {
    keep_image(2);
}

static void attach(tinyusb_event_t* event, void* arg) {
    (void)arg;
    if (event->id == TINYUSB_EVENT_ATTACHED) keep_image(1);
}

static void roll_back(void* arg) {
    (void)arg;
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

// The selection outlives a restart: an image without TinyUSB would otherwise find the USB-C pads taken
static void give_pads_back(void) {
    usb_wrap_ll_phy_select(&USB_WRAP, 1);
}

esp_err_t usb_start(bool (*command)(const char* line)) {
    app_command = command;
    storage_boot = boot_mode == STORAGE_BOOT;
    boot_mode = 0;
    control_port = storage_boot ? TINYUSB_CDC_ACM_0 : TINYUSB_CDC_ACM_1;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_RETURN_ON_ERROR(esp_timer_create(&(esp_timer_create_args_t){.callback = roll_back, .name = "rollback"}, &rollback), TAG, "timer");
        ESP_RETURN_ON_ERROR(esp_timer_start_once(rollback, 30 * 1000000), TAG, "timer");
    }
    // The USB-C pads are FSLS PHY 0, USB-Serial/JTAG's unless the OTG controller is mapped onto it
    usb_wrap_ll_phy_select(&USB_WRAP, 0);
    ESP_RETURN_ON_ERROR(esp_register_shutdown_handler(give_pads_back), TAG, "shutdown handler");
    // The MAC as USB-Serial/JTAG reports it, so a board keeps its name in both modes
    static char serial[18];
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_efuse_mac_get_default(mac), TAG, "MAC");
    snprintf(serial, sizeof serial, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    static const char* strings[4] = {"\x09\x04", "Espressif", "Tab5 SSH client"}; // English
    strings[3] = serial;
    tinyusb_config_t config = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .task = {.size = 4096, .priority = 5, .xCoreID = 1},
        .descriptor.full_speed_config = storage_boot ? storage_config : terminal_config,
        .descriptor.string = strings,
        .descriptor.string_count = 4,
        .event_cb = attach,
    };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&config), TAG, "driver");
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&(tinyusb_config_cdcacm_t){.cdc_port = TINYUSB_CDC_ACM_0}), TAG, "CDC 0");
    if (!storage_boot) ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&(tinyusb_config_cdcacm_t){.cdc_port = TINYUSB_CDC_ACM_1}), TAG, "CDC 1");
    ESP_RETURN_ON_ERROR(tinyusb_console_init(control_port), TAG, "console");
    return xTaskCreatePinnedToCore(control, "usb_control", 6144, NULL, 5, NULL, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
