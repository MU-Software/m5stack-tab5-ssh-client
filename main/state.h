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
    // IPv4 source address in network order, 0 for any: keeps the connection on one interface
    uint32_t local_address;
    // For each step of connecting
    int timeout_ms;
    // Replies to ssh_ping
    unsigned pongs;
    WOLFSSH_CTX* ctx;
    WOLFSSH* ssh;
    int fd;
    byte* key;
    word32 key_size;
    int error;
    byte host_key_sha256[WC_SHA256_DIGEST_SIZE];
} ssh_session;

// Logs on the SD card, rotated daily and at file_kb, oldest deleted past keep_mb or keep_days
typedef struct {
    bool enabled;
    int file_kb, keep_mb, keep_days;
    // Only warnings and the control commands' replies while a session runs
    bool quiet_sessions;
} log_settings;

// Keys shown before the host echoes them, once delay_ms passed without the echo
typedef struct {
    bool enabled;
    int delay_ms;
} echo_settings;

typedef struct {
    wifi_network* wifi;
    ssh_host* hosts;
    ssh_host* current_host;
    const char* terminal_type;
    terminal_size terminal_size;
    ssh_session* session;
    // Kept across boots: Wi-Fi stays off from boot, for LTE only
    bool wifi_off;
    log_settings log;
    // POSIX TZ
    char timezone[48];
    echo_settings echo;
    // Above and below the terminal
    struct {
        bool top, bottom;
    } bars;
    // An index of the "screen.refresh" setting's choices
    int refresh;
} global_state;
