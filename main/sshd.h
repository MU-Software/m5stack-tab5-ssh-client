// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wolfssh/ssh.h>

// One command per connection, as `ssh HOST COMMAND`
typedef struct {
    // Set by the caller; user NULL or empty: any user
    const char *authorized_keys, *user;
    WOLFSSH* ssh;
    int fd;
    char command[512];
    // Of the key offered last: once accepted, the one that logged in
    char fingerprint[64];
    bool refused;
} sshd_session;

#define SSHD_PUBLIC_KEY_SIZE 51

// Ed25519 as PKCS#8 PEM, as OpenSSL writes it: its length, or negative
int sshd_new_host_key(char* pem, size_t size);
// In SSH wire form
int sshd_public_key(const char* pem, uint8_t out[SSHD_PUBLIC_KEY_SIZE]);
WOLFSSH_CTX* sshd_new(const char* host_key);
// Until the client asks for a command; on failure the session is closed
int sshd_accept(WOLFSSH_CTX* ctx, sshd_session* s, int fd, int timeout_ms);
// Bytes, 0 for none yet, WS_EOF once the client sent all, or another WS_ error once it has gone
int sshd_read(sshd_session* s, void* buffer, size_t length);
void sshd_wait(const sshd_session* s, int timeout_ms);
// All of it; timeout_ms is per wait on a full window or socket
int sshd_write(sshd_session* s, const void* buffer, size_t length, int timeout_ms);
void sshd_close(sshd_session* s, int exit_status);
