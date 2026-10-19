// SPDX-License-Identifier: GPL-3.0-or-later
// LTE through the Stamp CAT-M (SIM7080G) on UART2, PPP on this chip; one task alone talks to the modem. G6 is also M5-Bus PC_TX
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>
#include <esp_netif.h>

#include "modem.h"
#include "network_rules.h"
#include "state.h"

// The PPP interface and the LTE task, which dials when on, checks the link and redials one whose bearer died; changed is
// called after each change of the view
esp_err_t lte_start(const global_state* global, void (*changed)(void));
void lte_view_now(lte_view* out);
esp_netif_t* lte_netif(void);
// For this boot; on also ends a wait, though no dial follows the last within 60 s
void lte_turn(bool on);
// Something failed over LTE or Wi-Fi: checked unless found good within 2 min or dead. When the verdict is due, 0 for no check;
// after a failed check the next waits 30 s, so it may come later
int64_t lte_suspect(void);
// An LTE session received bytes: the link lives
void lte_received(void);
// What the modem page shows as last read, without waiting
void lte_snapshot(modem_snapshot* out);
void lte_log_snapshot(void);
// lte status, modem read, modem at COMMAND, lte probe dead on|off: the LTE task runs them between dials and between a check's steps,
// and logs what came
bool lte_command(const char* line);
