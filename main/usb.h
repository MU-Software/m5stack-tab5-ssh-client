// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include <esp_err.h>

// command: the app's control commands, true when handled
esp_err_t usb_start(bool (*command)(const char* line));
// After the "sd on" command: CDC 0 is the control port and the SD card is mass storage, for one boot
int usb_storage_boot(void);
// A new OTA image is kept once USB is also up; otherwise it rolls back after 30 s, as after a crash before this
void usb_keep_image(void);
// Not usb_read/usb_write: the P4's ROM linker script defines those and silently wins
size_t usb_terminal_read(void* buffer, size_t size);
void usb_terminal_write(const void* buffer, size_t size);
