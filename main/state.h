// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
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
    const ssh_host* host;
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

// The Wi-Fi screen: the saved network it changes ("" for a new one), the network as typed, what came of the last step, and
// a delete waiting for a second press
typedef struct {
    char saved[33];
    wifi_network network;
    const char* result;
    bool deleting;
} wifi_edit;

enum { MAX_SESSIONS = 4 };

typedef enum { SESSION_CONNECTING, SESSION_CONNECTED, SESSION_ASKING, SESSION_STOPPED } session_status;

// An open SSH session as the screens show it
typedef struct {
    int number;
    // NULL while the first chooses one at boot
    const ssh_host* host;
    session_status status;
} session_info;

typedef struct {
    wifi_network* wifi;
    ssh_host* hosts;
    // The shown session's
    const ssh_host* current_host;
    const char* terminal_type;
    terminal_size terminal_size;
    // The open sessions, numbered from 1 in this order
    session_info sessions[MAX_SESSIONS];
    size_t session_count, shown_session;
    // Kept across boots: Wi-Fi or LTE stays off from boot
    bool wifi_off, lte_off;
    log_settings log;
    // POSIX TZ
    char timezone[48];
    echo_settings echo;
    // Above and below the terminal
    struct {
        bool top, bottom;
    } bars;
    // Index of the "bars.battery" setting's choices
    int battery;
    // On the terminal: drag pixels per wheel report or scrollback line, and Up/Down keys for a drag on the alternate screen
    // of a program that asked for no mouse reports
    struct {
        int wheel_px;
        bool arrows;
    } touch;
    // Control over SSH (remote.c): off by default; enabled needs the SD card's .ssh/authorized_keys as well
    struct {
        bool enabled, lte;
        // Only this SSH user name; empty for any, the key alone deciding
        char user[33];
    } remote;
    // Choosing a host at boot connects to the one chosen at this esp_timer time, 0 while nobody waits for it
    int64_t host_deadline_us;
    wifi_edit wifi_edit;
} global_state;
