// SPDX-License-Identifier: Apache-2.0
// The messages between the P4 and the C6 firmware of c6/, over esp_hosted's peer data: one id each way, the first byte
// the kind
#pragma once

#define C6_PROTOCOL 9

#define C6_TO_P4 0x54354336u // "T5C6"
// After P4_KEEP: the C6's protocol version
#define C6_HELLO 1
// Lines the C6 logged, from its boot on
#define C6_LOG 2
// The C6 joins the known networks itself: [state][reason of the last failure or drop][RSSI][SSID length][SSID], at each change
// and on P4_READY; held is the answer to P4_WIFI_HOLD when not connected
#define C6_WIFI_STATE 3
enum { C6_WIFI_STOPPED, C6_WIFI_SCANNING, C6_WIFI_JOINING, C6_WIFI_CONNECTED, C6_WIFI_NONE_IN_RANGE, C6_WIFI_JOIN_FAILED, C6_WIFI_HELD };
#define C6_WIFI_NAMES {"stopped", "scanning", "joining", "connected", "none in range", "join failed", "held"}
// The answer to P4_NVS: [sequence][esp_err_t, 4 bytes][body]
#define C6_NVS 4

#define P4_TO_C6 0x54355034u // "T5P4"
// The P4 reached the C6 through esp_hosted: an image the C6 has not kept yet becomes its own
#define P4_KEEP 1
// The known Wi-Fi networks, as the P4's configuration keeps them: [saved on][count], then for each in the order they are tried
// [SSID length][SSID][password length][password]
#define P4_WIFI_LIST 2
#define C6_NETWORKS  16
// The P4's network interface and handlers are up: the C6 replays its join and sends C6_WIFI_STATE
#define P4_READY 3
// [1] before the P4 scans or joins a network itself, [0] after: the C6 stops its scan or join, answers with C6_WIFI_STATE and
// starts no other until the release, or a minute later
#define P4_WIFI_HOLD 4
// [sequence][operation][body], one at a time, on the C6's default NVS partition. Names are [length][name], numbers little-endian,
// types nvs_type_t's
#define P4_NVS 5
enum {
    // [index, 2 bytes]: the entries from that one on, whole ones as many as fit, each [type][namespace][key][size, 4 bytes]; none
    // past the last
    C6_NVS_LIST,
    // [namespace][key][offset, 4 bytes]: [type][size, 4 bytes][the value from offset on, as much as fits]; a string with its
    // terminator, an integer as its bytes
    C6_NVS_GET,
    // [namespace][key][type][value], committed
    C6_NVS_SET,
    // [namespace][key], the whole namespace for an empty key, committed
    C6_NVS_ERASE,
    // [namespace][key][type][size, 4 bytes][offset, 4 bytes][piece], from offset 0 on: written and committed with the last piece
    C6_NVS_WRITE,
    // Every entry of every namespace, committed
    C6_NVS_CLEAR,
};
// Most of a request's body and of an answer's
#define C6_NVS_MESSAGE 1000
// The first protocol with P4_NVS, and with C6_NVS_WRITE and C6_NVS_CLEAR
#define C6_NVS_SINCE      6
#define C6_NVS_LOAD_SINCE 7
#define C6_NVS_U8         0x01
#define C6_NVS_I8         0x11
#define C6_NVS_U16        0x02
#define C6_NVS_I16        0x12
#define C6_NVS_U32        0x04
#define C6_NVS_I32        0x14
#define C6_NVS_U64        0x08
#define C6_NVS_I64        0x18
#define C6_NVS_STR        0x21
#define C6_NVS_BLOB       0x42

// The first protocol with Network Split: the C6 runs DHCP and keeps the address, ARP and ping; this chip takes the address and
// the TCP and UDP ports 49152-61439, and TCP port 22
#define C6_NW_SPLIT_SINCE 8
// From C6_NW_SPLIT_SINCE, at start before P4_WIFI_LIST: [name], the C6's DHCP hostname, kept in its NVS for the next lease
#define P4_HOSTNAME 6
#define C6_HOSTNAME 32

// From protocol 9. The C6 checks that its network reaches the internet: TCP port 443 of C6_INTERNET_ADDRESSES in turn through
// its own stack (ports 61440-65535), at each lease, on P4_INTERNET, and every 30 s while none answers; two failures in a row
// move it to another known network in reach, the failed one tried last for 10 minutes
#define C6_INTERNET_SINCE     9
#define C6_INTERNET_ADDRESSES {"1.1.1.1", "8.8.8.8", "9.9.9.9"}
// [sequence]: check now
#define P4_INTERNET 7
// [sequence][errno of the last address tried, 0 once one answered][milliseconds, 2 bytes][that address, 4 bytes]; the C6's own
// checks carry sequence 0
#define C6_INTERNET 5
// From protocol 9: [sequence][SSID length][SSID][password length][password]: the C6 leaves its network for this one, scanning
// every channel, and answers with C6_WIFI_JOINED once it is joined or failed
#define C6_JOIN_SINCE 9
#define P4_WIFI_JOIN  8
// [sequence][C6_JOIN_*][reason of the failure]
#define C6_WIFI_JOINED 6
enum { C6_JOIN_DONE, C6_JOIN_NOT_FOUND, C6_JOIN_FAILED, C6_JOIN_STOPPED, C6_JOIN_INTERRUPTED };

// After each SDIO card init the P4 raises esp_hosted's ESP_RESET (C6_SDIO_RESET) in HOST_TO_SLAVE_INTR: the C6, which may have
// run on through the P4's restart, closes the data path, restarts its SDIO counters and writes C6_RESET_DONE to its scratch
// register 0. Addresses as the host's esp_hosted gives them
#define C6_SCRATCH    0x6c
#define C6_INTERRUPT  0x8c
#define C6_SDIO_CLOSE 1
#define C6_SDIO_RESET 2
#define C6_RESET_DONE 0xa5
