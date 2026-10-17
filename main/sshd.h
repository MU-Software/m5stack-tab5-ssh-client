// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wolfssh/ssh.h>

// One command per connection, as `ssh HOST COMMAND` runs it, from a key on the authorized_keys list
typedef struct {
    // Set by the caller for sshd_accept; user NULL or empty for any user name
    const char *authorized_keys, *user;
    WOLFSSH* ssh;
    int fd;
    char command[512];
    // Of the key offered last: once accepted, the one that logged in
    char fingerprint[64];
    // A key or user name was turned down
    bool refused;
} sshd_session;

#define SSHD_PUBLIC_KEY_SIZE 51

// A new Ed25519 host key as PKCS#8 PEM, as OpenSSL writes it: its length, or a negative error
int sshd_new_host_key(char* pem, size_t size);
// The host key's public half in SSH wire form, as clients see it
int sshd_public_key(const char* pem, uint8_t out[SSHD_PUBLIC_KEY_SIZE]);
// For every session; NULL on error
WOLFSSH_CTX* sshd_new(const char* host_key);
// Until the client asks to run a command, at most timeout_ms; WS_SUCCESS, else the session is closed
int sshd_accept(WOLFSSH_CTX* ctx, sshd_session* s, int fd, int timeout_ms);
// Bytes, 0 for none yet, WS_EOF once the client sent all, or another WS_ error once it has gone
int sshd_read(sshd_session* s, void* buffer, size_t length);
// Until the socket has something to read, at most timeout_ms
void sshd_wait(const sshd_session* s, int timeout_ms);
// All of it, waiting at most timeout_ms each time the client's window or the socket is full; WS_SUCCESS or a WS_ error
int sshd_write(sshd_session* s, const void* buffer, size_t length, int timeout_ms);
void sshd_close(sshd_session* s, int exit_status);
