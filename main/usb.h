// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>

#include <esp_err.h>

esp_err_t usb_start(void);
// After the "sd on" command: CDC 0 is the control port and the SD card is mass storage, for one boot
int usb_storage_boot(void);
// A new OTA image is kept once USB is also up; otherwise it rolls back after 30 s, as after a crash before this
void usb_keep_image(void);
size_t usb_read(void* buffer, size_t size);
void usb_write(const void* buffer, size_t size);
