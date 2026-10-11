// SPDX-License-Identifier: GPL-3.0-or-later
#include "keyboard_usb.h"

#include <string.h>

#include <bsp/m5stack_tab5.h>
#include <esp_check.h>
#include <esp_log.h>
#include <usb/hid_host.h>
#include <usb/hid_usage_keyboard.h>

#include "keys.h"

static const char* TAG = "keyboard";
static uint8_t held[HID_KEYBOARD_KEY_MAX], modifiers;
static bool caps_lock;

static void send(uint8_t usage, GhosttyKeyAction action) {
    keys_push(&(key_event){usage, modifiers, caps_lock, action});
}

static bool contains(const uint8_t* keys, uint8_t usage) {
    return memchr(keys, usage, HID_KEYBOARD_KEY_MAX) != NULL;
}

// Boot reports list the keys held, so presses and releases come from the report before
static void report(const hid_keyboard_input_report_boot_t* r) {
    modifiers = r->modifier.val;
    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++)
        if (held[i] > HID_KEY_ERROR_UNDEFINED && !contains(r->key, held[i])) send(held[i], GHOSTTY_KEY_ACTION_RELEASE);
    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++)
        if (r->key[i] > HID_KEY_ERROR_UNDEFINED && !contains(held, r->key[i])) {
            caps_lock ^= r->key[i] == HID_KEY_CAPS_LOCK;
            send(r->key[i], GHOSTTY_KEY_ACTION_PRESS);
        }
    memcpy(held, r->key, sizeof held);
}

static void interface_event(hid_host_device_handle_t device, const hid_host_interface_event_t event, void* arg) {
    (void)arg;
    uint8_t data[sizeof(hid_keyboard_input_report_boot_t)];
    size_t size = 0;
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT && hid_host_device_get_raw_input_report_data(device, data, sizeof data, &size) == ESP_OK &&
        size == sizeof data)
        report((const hid_keyboard_input_report_boot_t*)data);
    if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        report(&(hid_keyboard_input_report_boot_t){0});
        hid_host_device_close(device);
    }
}

static void driver_event(hid_host_device_handle_t device, const hid_host_driver_event_t event, void* arg) {
    (void)arg;
    hid_host_dev_params_t params;
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED || hid_host_device_get_params(device, &params) != ESP_OK ||
        params.sub_class != HID_SUBCLASS_BOOT_INTERFACE || params.proto != HID_PROTOCOL_KEYBOARD ||
        hid_host_device_open(device, &(hid_host_device_config_t){.callback = interface_event}) != ESP_OK)
        return;
    // Sent even though keyboards often time out on it: some only report as boot devices once asked
    hid_class_request_set_protocol(device, HID_REPORT_PROTOCOL_BOOT);
    if (hid_host_device_start(device) == ESP_OK) ESP_LOGI(TAG, "attached");
}

esp_err_t keyboard_usb_start(void) {
    ESP_RETURN_ON_ERROR(bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true), TAG, "USB host");
    return hid_host_install(
        &(hid_host_driver_config_t){.create_background_task = true, .task_priority = 5, .stack_size = 4096, .core_id = 1, .callback = driver_event});
}
