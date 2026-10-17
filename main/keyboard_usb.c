// SPDX-License-Identifier: GPL-3.0-or-later
#include "keyboard_usb.h"

#include <string.h>

#include <esp_check.h>
#include <esp_log.h>
#include <usb/hid_host.h>
#include <usb/usb_host.h>
#include <usb/hid_usage_keyboard.h>

#include "keys.h"

static const char* TAG = "keyboard";
static uint8_t held[HID_KEYBOARD_KEY_MAX], modifiers;

static void send(uint8_t usage, GhosttyKeyAction action) {
    keys_push(&(key_event){.usage = usage, .modifiers = modifiers, .action = action});
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

static void run_host(void* arg) {
    (void)arg;
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

// What bsp_usb_host_start does, without the BSP's IO expander driver, which would reset the C6 (power.c powers the port)
esp_err_t keyboard_usb_start(void) {
    ESP_RETURN_ON_ERROR(usb_host_install(&(usb_host_config_t){.intr_flags = ESP_INTR_FLAG_LOWMED}), TAG, "USB host");
    ESP_RETURN_ON_FALSE(xTaskCreate(run_host, "usb host", 4096, NULL, 10, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "USB host task");
    return hid_host_install(
        &(hid_host_driver_config_t){.create_background_task = true, .task_priority = 5, .stack_size = 4096, .core_id = 1, .callback = driver_event});
}
