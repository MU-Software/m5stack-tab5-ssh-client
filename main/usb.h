// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

// command returns -1 for a line that is not the app's
esp_err_t usb_start(int (*command)(const char* line));
// A computer on USB-C
bool usb_attached(void);
// For one boot after "sd on": the SD card as mass storage, control on CDC 0
int usb_storage_boot(void);
// Not usb_read/usb_write: the P4's ROM linker script defines those and silently wins
size_t usb_terminal_read(void* buffer, size_t size);
void usb_terminal_write(const void* buffer, size_t size);
