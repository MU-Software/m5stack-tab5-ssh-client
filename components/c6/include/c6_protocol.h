// SPDX-License-Identifier: Apache-2.0
// The messages between the P4 and the C6 firmware of tools/c6, over esp_hosted's peer data: one id each way, the first byte
// the kind
#pragma once

#define C6_PROTOCOL 5

#define C6_TO_P4 0x54354336u // "T5C6"
// After P4_KEEP: the C6's protocol version
#define C6_HELLO 1
// Lines the C6 logged, from its boot on
#define C6_LOG 2
// The C6 joins the known networks itself: [state][reason of the last failure or drop][RSSI][SSID length][SSID], at each change
// and on P4_READY; held is the answer to P4_WIFI_HOLD when not connected
#define C6_WIFI_STATE 3
enum { C6_WIFI_STOPPED, C6_WIFI_SCANNING, C6_WIFI_JOINING, C6_WIFI_CONNECTED, C6_WIFI_NONE_IN_RANGE, C6_WIFI_JOIN_FAILED, C6_WIFI_HELD };

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

// After each SDIO card init the P4 raises esp_hosted's ESP_RESET (C6_SDIO_RESET) in HOST_TO_SLAVE_INTR: the C6, which may have
// run on through the P4's restart, closes the data path, restarts its SDIO counters and writes C6_RESET_DONE to its scratch
// register 0. Addresses as the host's esp_hosted gives them
#define C6_SCRATCH    0x6c
#define C6_INTERRUPT  0x8c
#define C6_SDIO_CLOSE 1
#define C6_SDIO_RESET 2
#define C6_RESET_DONE 0xa5
