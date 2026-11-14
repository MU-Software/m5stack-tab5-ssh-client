// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>

// name's A record, recursion desired: its length, 0 when out is short or name is not a host name
size_t dns_query(uint16_t id, const char* name, uint8_t* out, size_t size);
typedef enum { DNS_FOUND, DNS_NO_NAME, DNS_FAILED, DNS_NOT_OURS } dns_answer;
// address: the first A record, in network order as lwIP keeps it. DNS_NO_NAME: NXDOMAIN or no A record; DNS_FAILED: another error,
// truncated or malformed; DNS_NOT_OURS: not the answer to dns_query(id, name)
dns_answer dns_parse(uint16_t id, const char* name, const uint8_t* message, size_t length, uint32_t* address);
