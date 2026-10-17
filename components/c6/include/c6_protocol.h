// SPDX-License-Identifier: Apache-2.0
// The messages between the P4 and the C6 firmware of tools/c6, over esp_hosted's peer data: one id each way, the first byte
// the kind
#pragma once

#define C6_PROTOCOL 2

#define C6_TO_P4 0x54354336u // "T5C6"
// After P4_KEEP: the C6's protocol version
#define C6_HELLO 1
// Lines the C6 logged, from its boot on
#define C6_LOG 2

#define P4_TO_C6 0x54355034u // "T5P4"
// The P4 reached the C6 through esp_hosted: an image the C6 has not kept yet becomes its own
#define P4_KEEP 1
