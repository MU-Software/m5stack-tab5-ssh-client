// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

typedef enum { UPDATE_USB, UPDATE_NETWORK } update_path;

// Before anything that may crash: a new image rolls back unless kept within 30 s (installed over USB) or 60 s (network)
esp_err_t update_start(void);
// An app image into the other OTA slot, or the C6's image; the next restart boots it
esp_err_t update_install(bool c6, const uint8_t* image, size_t size);
// After the reply: the image installed over path is kept only once path works again
void update_restart(update_path path);
// USB came up, or a key logged in over the network
void update_works(update_path path);
// Once the image has run 10 s
void update_ran(void);
