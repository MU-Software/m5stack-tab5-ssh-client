// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

// command: the app's control commands, -1 for none of them
esp_err_t usb_start(int (*command)(const char* line));
// A computer uses the USB-C port
bool usb_attached(void);
// After the "sd on" command: CDC 0 is the control port and the SD card is mass storage, for one boot
int usb_storage_boot(void);
// Not usb_read/usb_write: the P4's ROM linker script defines those and silently wins
size_t usb_terminal_read(void* buffer, size_t size);
void usb_terminal_write(const void* buffer, size_t size);
