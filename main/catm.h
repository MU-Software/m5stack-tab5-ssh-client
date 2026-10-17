// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>
#include <esp_netif.h>

#include "modem.h"

// The PPP interface; the UART (G6 is also M5-Bus PC_TX) opens with the first command
esp_err_t catm_start(void);
// Ends a PPP session an earlier boot left open and checks the modem with read-only commands (but see catm_snapshot), or
// reports the PPP address
esp_err_t catm_status(void);
// catm_status's checks, then a dial, then an IP
esp_err_t catm_connect(void);
void catm_disconnect(void);
// PPP is up with an address
bool catm_connected(void);
// A modem answered or spoke since boot: one that falls silent later is there still
bool catm_answered(void);
esp_netif_t* catm_netif(void);
// What the modem page shows as last read, without waiting. The dial's checks and the network are read before each dial;
// the settings once a boot, after checks that find no back-off, with modem_commands' writes sent where modem_wanted
void catm_snapshot(modem_snapshot* out);
// All of it again, read-only apart from that once a boot; not during PPP
esp_err_t catm_read(void);
