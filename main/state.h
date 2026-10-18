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
    // The host's IPv4 address in network order, 0 to resolve its name here
    uint32_t address;
    // Called with the socket before it connects, if not NULL: binds it to one interface
    int (*prepare)(int fd, void* context);
    void* context;
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
    // Its key is not the one known_hosts has
    bool host_changed;
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

// The screen off after idle_minutes without a key or a touch (0: never), then light sleep while no computer uses USB-C,
// with the USB-A port's 5 V on only with usb_a; wifi_wake also wakes it for the C6's traffic (experimental)
typedef struct {
    int idle_minutes;
    bool usb_a, wifi_wake;
} sleep_settings;

// The SD card's .config/tab5-ssh/config.json; without it, the defaults
typedef enum { CONFIG_LOADED, CONFIG_MISSING, CONFIG_NO_CARD, CONFIG_INVALID } config_state;

enum { MAX_SESSIONS = 4 };

// A session's connection; asking: for a password or a host key, answered once it is shown
typedef enum { SESSION_DISCONNECTED, SESSION_CONNECTING, SESSION_CONNECTED, SESSION_RECONNECTING, SESSION_ASKING, SESSION_DELETING } session_status;

// A session as the SD card's .local/state/tab5-ssh/sessions.json keeps it: its tmux session on the host
typedef struct {
    int number;
    char alias[sizeof(((ssh_host*)0)->alias)], tmux[24];
} session_record;

// A session as the screens show it
typedef struct {
    int number;
    const char* alias;
    session_status status;
    // creating: its tmux session is being made; alert: a failure waits to be told
    bool shown, creating, alert;
} session_info;

typedef struct {
    wifi_network* wifi;
    ssh_host* hosts;
    // The shown session's, else the one shown last
    const ssh_host* current_host;
    const char* terminal_type;
    terminal_size terminal_size;
    // The sessions in the order of their numbers
    session_info sessions[MAX_SESSIONS];
    size_t session_count;
    // network.wifi.enabled and network.lte.enabled: on from boot
    bool wifi_enabled, lte_enabled;
    // Where the settings came from
    config_state config;
    log_settings log;
    // POSIX TZ
    char timezone[48];
    echo_settings echo;
    // Above and below the terminal
    struct {
        bool top, bottom;
    } bars;
    sleep_settings sleep;
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
} global_state;
