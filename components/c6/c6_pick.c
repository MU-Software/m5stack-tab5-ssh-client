// SPDX-License-Identifier: Apache-2.0
#include "c6_pick.h"

#include <stdbool.h>

int c6_next_network(const int8_t rssi[], const int64_t backoff_until[], const int64_t demoted_until[], int count, uint32_t skip, int64_t now) {
    for (int demoted = 0; demoted < 2; demoted++)
        for (int i = 0; i < count; i++)
            if (rssi[i] >= C6_RSSI_FLOOR && now >= backoff_until[i] && !(skip & 1u << i) && (now < demoted_until[i]) == (bool)demoted) return i;
    return -1;
}
