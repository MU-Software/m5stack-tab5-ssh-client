// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

#define C6_RSSI_FLOOR (-85)

// In list order, the demoted ones last; rssi INT8_MIN: out of reach; skip: a bit per network; -1 for none
int c6_next_network(const int8_t rssi[], const int64_t backoff_until[], const int64_t demoted_until[], int count, uint32_t skip, int64_t now);
