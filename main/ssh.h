// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "state.h"

// Provided by the application
int accept_new_host(const global_state* state, const char* name, const char* fingerprint, const char* line);

int ssh_open(const global_state* state);
// Bytes, WS_WANT_READ, or the end: WS_CHANNEL_CLOSED or another WS_ error
int ssh_read(const global_state* state, void* buffer, size_t length);
// Bytes, WS_WANT_WRITE (a full peer window opens only by reading), or a WS_ error
int ssh_write(const global_state* state, const void* buffer, size_t length);
// A request the host must answer, which counts in session->pongs: tells a slow network from a quiet program
void ssh_ping(const global_state* state);
void ssh_wait(const global_state* state, int timeout_ms);
void ssh_close(const global_state* state, int* exit_status);
