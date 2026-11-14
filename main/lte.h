// SPDX-License-Identifier: GPL-3.0-or-later
// Stamp CAT-M (SIM7080G); one task alone talks to the modem. G6 is also M5-Bus PC_TX
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>
#include <esp_netif.h>

#include "modem.h"
#include "network_rules.h"
#include "state.h"

// changed: after each view change
esp_err_t lte_start(const global_state* global, void (*changed)(void));
void lte_view_now(lte_view* out);
esp_netif_t* lte_netif(void);
// This boot only; on ends a wait, but no dial within 60 s of the last
void lte_turn(bool on);
// After a failure on either path: when the verdict is due, 0 for none (good within 2 min, or dead); later if a failed check delays the next 30 s
int64_t lte_suspect(void);
// An LTE session received bytes: the link lives
void lte_received(void);
// As last read, without waiting
void lte_snapshot(modem_snapshot* out);
void lte_log_snapshot(void);
// Queued: the LTE task runs them between dials and a check's steps, and logs what came
bool lte_command(const char* line);
