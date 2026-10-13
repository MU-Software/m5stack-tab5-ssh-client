// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <esp_err.h>

// The PPP interface; the UART (G6 is also M5-Bus PC_TX) opens with the first command
esp_err_t catm_start(void);
// Ends a PPP session an earlier boot left open and checks the modem with read-only commands, or reports the PPP address
esp_err_t catm_status(void);
// catm_status's checks, then a dial, then an IP
esp_err_t catm_connect(void);
void catm_disconnect(void);
