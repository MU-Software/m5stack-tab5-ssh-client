// SPDX-License-Identifier: Apache-2.0
// The messages between the P4 and the C6 firmware of tools/c6, over esp_hosted's peer data: one id each way, the first byte
// the kind
#pragma once

#define C6_PROTOCOL 3

#define C6_TO_P4 0x54354336u // "T5C6"
// After P4_KEEP: the C6's protocol version
#define C6_HELLO 1
// Lines the C6 logged, from its boot on
#define C6_LOG 2

#define P4_TO_C6 0x54355034u // "T5P4"
// The P4 reached the C6 through esp_hosted: an image the C6 has not kept yet becomes its own
#define P4_KEEP 1

// After each SDIO card init the P4 raises esp_hosted's ESP_RESET (C6_SDIO_RESET) in HOST_TO_SLAVE_INTR: the C6, which may have
// run on through the P4's restart, closes the data path, restarts its SDIO counters and writes C6_RESET_DONE to its scratch
// register 0. Addresses as the host's esp_hosted gives them
#define C6_SCRATCH    0x6c
#define C6_INTERRUPT  0x8c
#define C6_SDIO_CLOSE 1
#define C6_SDIO_RESET 2
#define C6_RESET_DONE 0xa5
