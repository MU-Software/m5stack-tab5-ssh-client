// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include <wolfssh/ssh.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include "modem.h"
#include "network_rules.h"

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
    // NULL: a shell
    const char* command;
    // IPv4, network order; 0: resolve the name here
    uint32_t address;
    // Before connect, if not NULL: binds the socket to one interface
    int (*prepare)(int fd, void* context);
    void* context;
    // Per step of connecting
    int timeout_ms;
    unsigned pongs;
    WOLFSSH_CTX* ctx;
    WOLFSSH* ssh;
    int fd;
    byte* key;
    word32 key_size;
    int error;
    byte host_key_sha256[WC_SHA256_DIGEST_SIZE];
    bool host_changed;
} ssh_session;

typedef struct {
    bool enabled;
    int file_kb, keep_mb, keep_days;
    // Warnings and control replies only, while a session runs
    bool quiet_sessions;
} log_settings;

// Local echo after delay_ms without the host's
typedef struct {
    bool enabled;
    int delay_ms;
} echo_settings;

// idle_minutes 0: never; usb_a: USB-A's 5 V stays on in sleep; wifi_wake: the C6's traffic wakes it (experimental)
typedef struct {
    int idle_minutes;
    bool usb_a, wifi_wake;
} sleep_settings;

typedef enum { CONFIG_LOADED, CONFIG_MISSING, CONFIG_NO_CARD, CONFIG_INVALID } config_state;

enum { MAX_SESSIONS = 4 };

// ASKING: for a password or a host key
typedef enum { SESSION_DISCONNECTED, SESSION_CONNECTING, SESSION_CONNECTED, SESSION_RECONNECTING, SESSION_ASKING, SESSION_DELETING } session_status;

// As sessions.json keeps it
typedef struct {
    int number;
    char alias[sizeof(((ssh_host*)0)->alias)], tmux[24];
} session_record;

typedef struct {
    int number;
    const char* alias;
    session_status status;
    // alert: a failure to tell
    bool shown, creating, alert;
} session_info;

typedef struct {
    // "none", "Wi-Fi" or "LTE"
    const char* path;
    bool wifi_on, wifi_joined, lte_on;
    network_health wifi_internet;
    lte_state lte;
    char ssid[33], wifi_address[16], lte_address[16];
    int8_t rssi;
} network_info;

// IDLE: on a charger, not charging; MISSING: no pack, a charger in
typedef enum { PACK_CHARGING, PACK_DISCHARGING, PACK_IDLE, PACK_MISSING } pack_flow;
typedef struct {
    bool known;
    pack_flow flow;
    int millivolts;
} pack_reading;

typedef struct {
    wifi_network* wifi;
    ssh_host* hosts;
    const char* terminal_type;
    terminal_size terminal_size;
    // In number order
    session_info sessions[MAX_SESSIONS];
    size_t session_count;
    // network.*.enabled: on from boot
    bool wifi_enabled, lte_enabled;
    config_state config;
    log_settings log;
    // POSIX TZ
    char timezone[48];
    echo_settings echo;
    sleep_settings sleep;
    // "bars.battery" choice index
    int battery;
    // wheel_px: drag pixels per wheel report or line; arrows: Up/Down for drags on an alternate screen without mouse reports
    struct {
        int wheel_px;
        bool arrows;
    } touch;
    // remote.c; enabled also needs .ssh/authorized_keys
    struct {
        bool enabled, lte;
        // Empty: any, the key alone deciding
        char user[33];
    } remote;
    // Refreshed only while the screen is on
    network_info network;
    const modem_snapshot* modem;
    pack_reading pack;
} global_state;
