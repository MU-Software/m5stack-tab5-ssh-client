// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

#include <wolfssh/ssh.h>
#include <wolfssl/wolfcrypt/sha256.h>

typedef struct ssh_host {
    char alias[254], hostname[254], user[33], identity_file[128];
    uint16_t port;
    struct ssh_host* next;
} ssh_host;

typedef struct {
    uint32_t columns, rows, width_pixels, height_pixels;
} terminal_size;

typedef struct wifi_network {
    char ssid[33], password[65];
    struct wifi_network* next;
} wifi_network;

typedef struct {
    // Set by the caller, NULL once used
    const char *known_hosts, *private_key, *password;
    // Run with the terminal instead of a shell, if not NULL
    const char* command;
    WOLFSSH_CTX* ctx;
    WOLFSSH* ssh;
    int fd;
    byte* key;
    word32 key_size;
    int error;
    byte host_key_sha256[WC_SHA256_DIGEST_SIZE];
} ssh_session;

typedef struct {
    wifi_network* wifi;
    ssh_host* hosts;
    ssh_host* current_host;
    const char* terminal_type;
    terminal_size terminal_size;
    ssh_session* session;
} global_state;
