// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>

// An RFC 1035 query for name's A record, recursion desired, into out: its length, 0 when it does not fit or name is not a valid host name
size_t dns_query(uint16_t id, const char* name, uint8_t* out, size_t size);
typedef enum { DNS_FOUND, DNS_NO_NAME, DNS_FAILED, DNS_NOT_OURS } dns_answer;
// message as the answer to dns_query(id, name): DNS_FOUND with the first A record's address as lwIP keeps one (the four bytes in
// network order in memory), DNS_NO_NAME for NXDOMAIN or no A record, DNS_FAILED for another error code, a truncated or malformed
// answer, DNS_NOT_OURS when it is not a response to that question (id, QR bit, question name/type/class)
dns_answer dns_parse(uint16_t id, const char* name, const uint8_t* message, size_t length, uint32_t* address);
