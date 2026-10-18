// SPDX-License-Identifier: GPL-3.0-or-later
#include "dns_message.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

static unsigned u16(const uint8_t* p) {
    return p[0] << 8 | p[1];
}

// name as wire labels into out: their length, 0 when it does not fit or a label is empty or too long
static size_t encode(const char* name, uint8_t* out, size_t size) {
    size_t n = strlen(name), start = 0;
    if (n > 0 && name[n - 1] == '.') n--;
    if (n == 0 || n > 253 || n + 2 > size) return 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && name[i] != '.') {
            out[i + 1] = name[i];
            continue;
        }
        if (i == start || i - start > 63) return 0;
        out[start] = i - start;
        start = i + 1;
    }
    out[n + 1] = 0;
    return n + 2;
}

// The name at *at, pointers followed, as wire labels into out[255], *at past it: its length, 0 when malformed. Each pointer
// must go back before the labels read so far
static size_t name_at(const uint8_t* m, size_t length, size_t* at, uint8_t* out) {
    size_t p = *at, start = p, end = 0, n = 0;
    for (int jumps = 0; p < length;) {
        uint8_t c = m[p];
        if (c >= 0xc0) {
            if (p + 1 >= length) return 0;
            size_t to = (c & 0x3f) << 8 | m[p + 1];
            if (to >= start || ++jumps > 32) return 0;
            if (end == 0) end = p + 2;
            start = p = to;
        } else if (c >= 0x40 || p + 1 + c > length || n + 1 + c > 255) {
            return 0;
        } else {
            memcpy(out + n, m + p, c + 1);
            n += c + 1;
            p += c + 1;
            if (c == 0) {
                *at = end != 0 ? end : p;
                return n;
            }
        }
    }
    return 0;
}

static bool same(const uint8_t* a, const uint8_t* b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (tolower(a[i]) != tolower(b[i])) return false;
    return true;
}

size_t dns_query(uint16_t id, const char* name, uint8_t* out, size_t size) {
    size_t n = size > 16 ? encode(name, out + 12, size - 16) : 0;
    if (n == 0) return 0;
    memcpy(out, (uint8_t[12]){id >> 8, id & 0xff, 1, 0, 0, 1}, 12);
    memcpy(out + 12 + n, (uint8_t[4]){0, 1, 0, 1}, 4);
    return n + 16;
}

dns_answer dns_parse(uint16_t id, const char* name, const uint8_t* m, size_t length, uint32_t* address) {
    uint8_t want[256], got[255];
    size_t at = 12, n = encode(name, want, sizeof want);
    if (n == 0 || length < 12 || u16(m) != id || !(m[2] & 0x80) || u16(m + 4) != 1) return DNS_NOT_OURS;
    size_t got_n = name_at(m, length, &at, got);
    if (got_n == 0 || at + 4 > length) return DNS_FAILED;
    if (got_n != n || !same(got, want, n) || u16(m + at) != 1 || u16(m + at + 2) != 1) return DNS_NOT_OURS;
    if (m[2] & 0x02) return DNS_FAILED;
    if ((m[3] & 0x0f) == 3) return DNS_NO_NAME;
    if ((m[3] & 0x0f) != 0) return DNS_FAILED;
    at += 4;
    for (unsigned i = u16(m + 6); i > 0; i--) {
        if (name_at(m, length, &at, got) == 0 || at + 10 > length) return DNS_FAILED;
        unsigned size = u16(m + at + 8);
        bool a = u16(m + at) == 1 && u16(m + at + 2) == 1 && size == 4;
        at += 10;
        if (at + size > length) return DNS_FAILED;
        if (a) {
            memcpy(address, m + at, 4);
            return DNS_FOUND;
        }
        at += size;
    }
    return DNS_NO_NAME;
}
