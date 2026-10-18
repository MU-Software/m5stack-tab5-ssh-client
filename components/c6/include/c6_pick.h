// SPDX-License-Identifier: Apache-2.0
// Which known network the C6 tries next
#pragma once
#include <stdint.h>

#define C6_RSSI_FLOOR (-85)

// Network i of count, in list order: seen at rssi[i] (INT8_MIN out of reach) at least C6_RSSI_FLOOR, past backoff_until[i]
// and not in the skip bits, those past demoted_until[i] before the others; -1 for none
int c6_next_network(const int8_t rssi[], const int64_t backoff_until[], const int64_t demoted_until[], int count, uint32_t skip, int64_t now);
