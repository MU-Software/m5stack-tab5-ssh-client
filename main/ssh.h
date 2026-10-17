// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "state.h"

// Provided by the application
int accept_new_host(const ssh_session* session, const char* name, const char* fingerprint, const char* line);

// To session->host, with the state's terminal type and size
int ssh_open(const global_state* state, ssh_session* session);
// Bytes, WS_WANT_READ, or the end: WS_CHANNEL_CLOSED or another WS_ error
int ssh_read(ssh_session* session, void* buffer, size_t length);
// Bytes, WS_WANT_WRITE (a full peer window opens only by reading), or a WS_ error
int ssh_write(ssh_session* session, const void* buffer, size_t length);
// A request the host must answer, which counts in session->pongs: tells a slow network from a quiet program
void ssh_ping(ssh_session* session);
// Tells the host the state's terminal size
void ssh_resize(const global_state* state, ssh_session* session);
void ssh_wait(ssh_session* session, int timeout_ms);
void ssh_close(ssh_session* session, int* exit_status);
