// SPDX-License-Identifier: GPL-3.0-or-later
#include "keyboard_tab5.h"

#include <driver/i2c_master.h>
#include <esp_heap_caps.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>

#include "keys.h"

#define ADDRESS 0x6d
#define SHIFT   0x80
#define SYM     (3 << 4 | 0)
#define CTRL    (4 << 4 | 0)
#define ALT     (4 << 4 | 1)

static const char* TAG = "keyboard";

// HID usages of each key alone and with Sym (SHIFT: typed with Shift), as the accessory's own firmware maps them
// (m5stack/M5Tab5-Keyboard-Internal-FW, user_keyboard_handle.c), but Aa as CapsLock and Sym with 1…0 - + as F1–F12
// clang-format off
static const uint8_t layers[5][14][2] = {
    {{0x29, 0x29}, {0x1e, 0x3a}, {0x1f, 0x3b}, {0x20, 0x3c}, {0x21, 0x3d}, {0x22, 0x3e}, {0x23, 0x3f}, {0x24, 0x40}, {0x25, 0x41}, {0x26, 0x42}, {0x27, 0x43}, {0x2d, 0x44}, {0xae, 0x45}, {0x4c, 0x4c}},
    {{0x35, 0xb5}, {0x9e, 0xb8}, {0x9f, 0x9f}, {0xa0, 0xa0}, {0xa1, 0xa1}, {0xa2, 0xa2}, {0xa3, 0xa3}, {0xa4, 0xa4}, {0xa5, 0x38}, {0xa6, 0xb6}, {0xa7, 0xb7}, {0x2f, 0xaf}, {0x30, 0xb0}, {0x31, 0xb1}},
    {{0x2b, 0x2b}, {0x14, 0x94}, {0x1a, 0x9a}, {0x08, 0x88}, {0x15, 0x95}, {0x17, 0x97}, {0x1c, 0x9c}, {0x18, 0x98}, {0x0c, 0x8c}, {0x12, 0x92}, {0x13, 0x93}, {0x33, 0xb3}, {0x34, 0xb4}, {0x2a, 0x2a}},
    {{0x00, 0x00}, {0x39, 0x39}, {0x04, 0x84}, {0x16, 0x96}, {0x07, 0x87}, {0x09, 0x89}, {0x0a, 0x8a}, {0x0b, 0x8b}, {0x0d, 0x8d}, {0x0e, 0x8e}, {0x0f, 0x8f}, {0x52, 0x52}, {0xad, 0x2e}, {0x28, 0x28}},
    {{0x00, 0x00}, {0x00, 0x00}, {0x1d, 0x9d}, {0x1b, 0x9b}, {0x06, 0x86}, {0x19, 0x99}, {0x05, 0x85}, {0x11, 0x91}, {0x10, 0x90}, {0x37, 0x36}, {0x50, 0x50}, {0x51, 0x51}, {0x4f, 0x4f}, {0x2c, 0x2c}},
};
// clang-format on

static i2c_master_dev_handle_t device;
static uint8_t modifiers, down[5][14];
static bool sym;

// An event of normal mode: bit 7 pressed, bits 6:4 row, bits 3:0 column
static void handle(uint8_t event) {
    bool pressed = event & 0x80;
    uint8_t key = event & 0x7f, row = key >> 4, col = key & 0x0f;
    if (row >= 5 || col >= 14) return;
    if (key == SYM) sym = pressed;
    if (key == CTRL || key == ALT) modifiers = (modifiers & ~(key == CTRL ? 0x01 : 0x04)) | (pressed ? (key == CTRL ? 0x01 : 0x04) : 0);
    if (key == SYM || key == CTRL || key == ALT) return;
    uint8_t code = pressed ? (down[row][col] = layers[row][col][sym]) : down[row][col];
    keys_push(&(key_event){.usage = code & ~SHIFT,
                           .modifiers = (uint8_t)(modifiers | (code & SHIFT ? 0x02 : 0)),
                           .action = pressed ? GHOSTTY_KEY_ACTION_PRESS : GHOSTTY_KEY_ACTION_RELEASE});
}

static esp_err_t write(uint8_t reg, uint8_t value) {
    return i2c_master_transmit(device, (uint8_t[]){reg, value}, 2, 50);
}

static void poll(void* bus) {
    bool attached = false;
    for (;;) {
        uint8_t event = 0xff;
        if (!attached) {
            // Normal mode (register 0x10), with what was queued before flushed (0x02)
            attached = i2c_master_probe(bus, ADDRESS, 50) == ESP_OK && write(0x10, 0) == ESP_OK && write(0x02, 0) == ESP_OK;
            if (attached) ESP_LOGI(TAG, "Tab5 keyboard attached");
        } else if (i2c_master_transmit_receive(device, (uint8_t[]){0x20}, 1, &event, 1, 50) != ESP_OK) {
            attached = sym = false;
            modifiers = 0;
            keys_push(&(key_event){.action = GHOSTTY_KEY_ACTION_RELEASE});
        }
        if (event != 0xff)
            handle(event);
        else
            vTaskDelay(pdMS_TO_TICKS(attached ? 20 : 1000));
    }
}

esp_err_t keyboard_tab5_start(void) {
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&(i2c_master_bus_config_t){.i2c_port = -1,
                                                                      .sda_io_num = 0,
                                                                      .scl_io_num = 1,
                                                                      .clk_source = I2C_CLK_SRC_DEFAULT,
                                                                      .glitch_ignore_cnt = 7,
                                                                      .flags.enable_internal_pullup = true},
                                           &bus),
                        TAG, "I2C");
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &(i2c_device_config_t){.device_address = ADDRESS, .scl_speed_hz = 100000}, &device), TAG,
                        "device");
    // Stack in PSRAM: a task there must never write flash
    return xTaskCreatePinnedToCoreWithCaps(poll, "tab5 keyboard", 3072, bus, 5, NULL, 1, MALLOC_CAP_SPIRAM) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
