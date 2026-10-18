// SPDX-License-Identifier: GPL-3.0-or-later
#include "dns_message.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

// Response id 0x1234 with QR and RD, flags then RA and RCODE, one question and `an` answer records
#define HEADER(flags, rcode, an) 0x12, 0x34, flags, 0x80 | (rcode), 0, 1, 0, an, 0, 0, 0, 0
#define EXAMPLE_COM              7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0
#define QUESTION                 EXAMPLE_COM, 0, 1, 0, 1
#define TTL_A(a)                 0, 1, 0, 1, 0, 0, 0x0e, 0x10, 0, 4, 93, 184, 216, a

static dns_answer parsed(const char* name, const uint8_t* m, size_t length, uint32_t* address) {
    *address = 0;
    return dns_parse(0x1234, name, m, length, address);
}

static int is(uint32_t address, uint8_t last) {
    return memcmp(&address, (uint8_t[]){93, 184, 216, last}, 4) == 0;
}

int main(void) {
    uint8_t q[300];
    const uint8_t example[] = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0, QUESTION};
    check(dns_query(0x1234, "example.com", q, sizeof q) == sizeof example && memcmp(q, example, sizeof example) == 0, "query bytes for example.com");
    check(dns_query(0x1234, "example.com.", q, sizeof q) == sizeof example && memcmp(q, example, sizeof example) == 0, "a trailing dot");
    check(dns_query(0x1234, "example.com", q, sizeof example) == sizeof example, "a buffer just large enough");
    check(dns_query(0x1234, "example.com", q, sizeof example - 1) == 0 && dns_query(0x1234, "example.com", q, 16) == 0, "a buffer too small");

    char name[300];
    memset(name, 'a', sizeof name);
    name[63] = name[127] = name[191] = '.';
    name[253] = 0;
    check(dns_query(1, name, q, sizeof q) == 253 + 18, "253 characters, labels of 63");
    name[253] = 'a', name[254] = 0;
    check(dns_query(1, name, q, sizeof q) == 0, "254 characters");
    name[63] = 0;
    check(dns_query(1, name, q, sizeof q) == 63 + 18, "a label of 63");
    name[63] = 'a', name[64] = 0;
    check(dns_query(1, name, q, sizeof q) == 0, "a label of 64");
    const char* const bad[] = {"", ".", "a..b", ".a", "a.b.."};
    size_t sum = 0;
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        sum += dns_query(1, bad[i], q, sizeof q);
    check(sum == 0, "an empty name or label");

    uint32_t address;
    const uint8_t one[] = {HEADER(0x81, 0, 1), QUESTION, 0xc0, 12, TTL_A(34)};
    check(parsed("example.com", one, sizeof one, &address) == DNS_FOUND && is(address, 34), "one A, in network order");
    check(parsed("Example.COM.", one, sizeof one, &address) == DNS_FOUND, "the name case-insensitively");
    const uint8_t upper[] = {HEADER(0x81, 0, 1), 7, 'E', 'X', 'A', 'M', 'P', 'L', 'E', 3, 'c', 'o', 'M', 0, 0, 1, 0, 1, 0xc0, 12, TTL_A(34)};
    check(parsed("example.com", upper, sizeof upper, &address) == DNS_FOUND, "the question in another case");

    const uint8_t several[] = {HEADER(0x81, 0, 3), QUESTION, 0xc0, 12, TTL_A(1), 0xc0, 12, TTL_A(2), 0xc0, 12, TTL_A(3)};
    check(parsed("example.com", several, sizeof several, &address) == DNS_FOUND && is(address, 1), "several A: the first");

    // A CNAME to www.example.com, its target a pointer into the question, then the A owned by the target: two jumps
    const uint8_t cname[] = {HEADER(0x81, 0, 2), QUESTION, 0xc0, 12, 0, 5, 0, 1, 0, 0, 0, 60, 0, 6, 3, 'w', 'w', 'w', 0xc0, 12, 0xc0, 41, TTL_A(7)};
    check(parsed("example.com", cname, sizeof cname, &address) == DNS_FOUND && is(address, 7), "a CNAME then its A through pointers");
    int found = 0;
    for (size_t n = 0; n < sizeof cname; n++)
        found += parsed("example.com", cname, n, &address) == DNS_FOUND;
    check(found == 0, "never found when cut anywhere");
    check(parsed("example.com", cname, sizeof cname - 1, &address) == DNS_FAILED && parsed("example.com", cname, 31, &address) == DNS_FAILED &&
              parsed("example.com", cname, 20, &address) == DNS_FAILED && parsed("example.com", cname, 11, &address) == DNS_NOT_OURS,
          "cut in the answers or question: failed; in the header: not ours");

    const uint8_t nxdomain[] = {HEADER(0x81, 3, 0), QUESTION};
    check(parsed("example.com", nxdomain, sizeof nxdomain, &address) == DNS_NO_NAME, "NXDOMAIN");
    const uint8_t none[] = {HEADER(0x81, 0, 0), QUESTION};
    check(parsed("example.com", none, sizeof none, &address) == DNS_NO_NAME, "NOERROR without answers");
    const uint8_t cname_only[] = {HEADER(0x81, 0, 1), QUESTION, 0xc0, 12, 0, 5, 0, 1, 0, 0, 0, 60, 0, 6, 3, 'w', 'w', 'w', 0xc0, 12};
    check(parsed("example.com", cname_only, sizeof cname_only, &address) == DNS_NO_NAME, "NOERROR with a CNAME but no A");
    const uint8_t aaaa[] = {HEADER(0x81, 0, 1),
                            QUESTION,
                            0xc0,
                            12,
                            0,
                            28,
                            0,
                            1,
                            0,
                            0,
                            0,
                            60,
                            0,
                            16,
                            0x26,
                            6,
                            0x28,
                            0,
                            0x02,
                            0x20,
                            0,
                            1,
                            0x2,
                            0x48,
                            0x18,
                            0x93,
                            0x25,
                            0xc8,
                            0x19,
                            0x46};
    check(parsed("example.com", aaaa, sizeof aaaa, &address) == DNS_NO_NAME, "an AAAA is no A");
    const uint8_t servfail[] = {HEADER(0x81, 2, 0), QUESTION};
    check(parsed("example.com", servfail, sizeof servfail, &address) == DNS_FAILED, "SERVFAIL");
    const uint8_t truncated[] = {HEADER(0x83, 0, 1), QUESTION, 0xc0, 12, TTL_A(34)};
    check(parsed("example.com", truncated, sizeof truncated, &address) == DNS_FAILED, "TC bit");

    uint8_t m[sizeof one];
    memcpy(m, one, sizeof m);
    check(dns_parse(0x1235, "example.com", m, sizeof m, &address) == DNS_NOT_OURS, "another id");
    m[2] = 0x01;
    check(parsed("example.com", m, sizeof m, &address) == DNS_NOT_OURS, "a query, QR clear");
    check(parsed("example.org", one, sizeof one, &address) == DNS_NOT_OURS, "another name");
    check(parsed("www.example.com", one, sizeof one, &address) == DNS_NOT_OURS, "a longer name");
    check(parsed("example..com", one, sizeof one, &address) == DNS_NOT_OURS, "an invalid name");
    memcpy(m, one, sizeof m);
    m[26] = 28;
    check(parsed("example.com", m, sizeof m, &address) == DNS_NOT_OURS, "an AAAA question");
    memcpy(m, one, sizeof m);
    m[28] = 3;
    check(parsed("example.com", m, sizeof m, &address) == DNS_NOT_OURS, "another class");
    memcpy(m, one, sizeof m);
    m[5] = 0;
    check(parsed("example.com", m, sizeof m, &address) == DNS_NOT_OURS, "no question");

    memcpy(m, one, sizeof m);
    m[30] = 29;
    check(parsed("example.com", m, sizeof m, &address) == DNS_FAILED, "a pointer to itself");
    m[30] = 0xff;
    check(parsed("example.com", m, sizeof m, &address) == DNS_FAILED, "a pointer out of range");
    m[29] = 0xc1, m[30] = 0;
    check(parsed("example.com", m, sizeof m, &address) == DNS_FAILED, "a pointer past the end");
    m[29] = 0x80, m[30] = 12;
    check(parsed("example.com", m, sizeof m, &address) == DNS_FAILED, "a reserved label type");
    // www then a pointer back to www: a loop through a label
    const uint8_t loop[] = {HEADER(0x81, 0, 1), QUESTION, 3, 'w', 'w', 'w', 0xc0, 29, TTL_A(34)};
    check(parsed("example.com", loop, sizeof loop, &address) == DNS_FAILED, "a pointer loop through a label");
    const uint8_t forward[] = {HEADER(0x81, 0, 1), QUESTION, 0xc0, 31, 0, TTL_A(34)};
    check(parsed("example.com", forward, sizeof forward, &address) == DNS_FAILED, "a pointer forward");
    return failures != 0;
}
