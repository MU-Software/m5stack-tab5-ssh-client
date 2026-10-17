// SPDX-License-Identifier: GPL-3.0-or-later
#include "c6_nvs.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "c6_protocol.h"
#include "control.h"

#define NOT_FOUND 0x1102
#define TIMEOUT   0x107

static int failures;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

// The C6, as tools/c6 answers
typedef struct {
    char space[16], key[16];
    uint8_t type, value[4096];
    size_t size;
} entry;
static entry store[32];
static int stored, exchanges, writes, fail_with;
static uint8_t pending[4096];
static size_t answer_limit = C6_NVS_MESSAGE;
static bool malformed;

static void put_number(uint8_t* out, uint32_t n) {
    for (int i = 0; i < 4; i++)
        out[i] = (uint8_t)(n >> 8 * i);
}

static const uint8_t* take_name(const uint8_t* at, char name[16]) {
    snprintf(name, 16, "%.*s", at[0], (const char*)at + 1);
    return at + 1 + at[0];
}

static entry* find(const char* space, const char* key) {
    for (int i = 0; i < stored; i++)
        if (strcmp(store[i].space, space) == 0 && strcmp(store[i].key, key) == 0) return &store[i];
    return NULL;
}

static int fake_exchange(const uint8_t* request, size_t request_length, uint8_t* answer, size_t* answer_length) {
    char space[16], key[16];
    size_t limit = *answer_length < answer_limit ? *answer_length : answer_limit, n = 0;
    exchanges++;
    if (fail_with != 0) return fail_with;
    if (request[0] == C6_NVS_LIST) {
        for (int i = request[1] | request[2] << 8; i < stored; i++) {
            const entry* e = &store[i];
            size_t s = strlen(e->space), k = strlen(e->key);
            if (n + 7 + s + k > limit) break;
            answer[n++] = e->type;
            answer[n++] = (uint8_t)s;
            memcpy(answer + n, e->space, s);
            answer[n += s] = (uint8_t)k;
            memcpy(answer + ++n, e->key, k);
            put_number(answer + (n += k), (uint32_t)e->size);
            n += 4;
        }
        *answer_length = n;
        return 0;
    }
    const uint8_t* at = take_name(take_name(request + 1, space), key);
    entry* e = find(space, key);
    if (request[0] == C6_NVS_GET) {
        uint32_t offset = at[0] | at[1] << 8 | at[2] << 16 | (uint32_t)at[3] << 24;
        if (e == NULL) return NOT_FOUND;
        answer[0] = e->type;
        put_number(answer + 1, (uint32_t)e->size);
        n = offset < e->size ? e->size - offset : 0;
        if (n > limit - 5) n = limit - 5;
        memcpy(answer + 5, e->value + offset, n);
        *answer_length = malformed ? 3 : 5 + n;
        return 0;
    }
    *answer_length = 0;
    if (request[0] == C6_NVS_SET || request[0] == C6_NVS_WRITE) {
        const uint8_t* value = at + 1;
        size_t size = request_length - (size_t)(at + 1 - request);
        if (request[0] == C6_NVS_WRITE) {
            uint32_t total = at[1] | at[2] << 8 | at[3] << 16 | (uint32_t)at[4] << 24,
                     offset = at[5] | at[6] << 8 | at[7] << 16 | (uint32_t)at[8] << 24;
            size_t piece = request_length - (size_t)(at + 9 - request);
            writes++;
            memcpy(pending + offset, at + 9, piece);
            if (offset + piece < total) return 0;
            value = pending, size = total;
        }
        if (e == NULL) e = &store[stored++];
        snprintf(e->space, sizeof e->space, "%s", space);
        snprintf(e->key, sizeof e->key, "%s", key);
        e->type = at[0];
        memcpy(e->value, value, size);
        if (e->type == C6_NVS_STR) e->value[size++] = 0;
        e->size = size;
        return 0;
    }
    if (request[0] == C6_NVS_CLEAR) {
        stored = 0;
        return 0;
    }
    int erased = 0;
    for (int i = 0; i < stored; i++)
        if (strcmp(store[i].space, space) == 0 && (key[0] == 0 || strcmp(store[i].key, key) == 0)) {
            memmove(&store[i], &store[i + 1], (size_t)(stored - i - 1) * sizeof store[0]);
            stored--, i--, erased++;
        }
    return erased > 0 ? 0 : NOT_FOUND;
}

static const char* error_name(int error) {
    return error == 0 ? "ESP_OK" : error == NOT_FOUND ? "ESP_ERR_NVS_NOT_FOUND" : error == TIMEOUT ? "ESP_ERR_TIMEOUT" : "?";
}

static char out[32768];
static size_t out_length;

static void write_out(void* context, const char* text, size_t length) {
    (void)context;
    if (out_length + length < sizeof out) memcpy(out + out_length, text, length), out_length += length;
    out[out_length] = 0;
}

static const char* input;
static size_t input_left;

// A few bytes at a time
static int read_input(void* context, void* buffer, size_t length) {
    size_t n = input_left < 7 ? input_left : 7;
    (void)context;
    if (n > length) n = length;
    memcpy(buffer, input, n);
    input += n, input_left -= n;
    return (int)n;
}

static void feed(const char* text) {
    input = text, input_left = strlen(text);
}

static c6_nvs_io io = {"c6 nvs", C6_PROTOCOL, fake_exchange, error_name, write_out, read_input, NULL};
static int status;

static const char* run(const char* arguments) {
    out_length = 0;
    out[0] = 0;
    status = c6_nvs_command(arguments, &io);
    return out;
}

static void expect(const char* arguments, const char* reply, int expected_status) {
    char what[160];
    run(arguments);
    snprintf(what, sizeof what, "c6 nvs%s", arguments);
    check(strcmp(out, reply) == 0 && status == expected_status, what);
    if (strcmp(out, reply) != 0) printf("     got %s", out);
}

int main(void) {
    expect(" set test n u32 42", "c6 nvs ESP_OK\n", 0);
    expect(" set test neg i8 -128", "c6 nvs ESP_OK\n", 0);
    expect(" set test big u64 18446744073709551615", "c6 nvs ESP_OK\n", 0);
    expect(" set test min i64 -9223372036854775808", "c6 nvs ESP_OK\n", 0);
    expect(" set test word i16 -2", "c6 nvs ESP_OK\n", 0);
    expect(" set test s str hello \\ world\x01", "c6 nvs ESP_OK\n", 0);
    expect(" set test empty str", "c6 nvs ESP_OK\n", 0);
    expect(" set test b blob 00Ff10", "c6 nvs ESP_OK\n", 0);
    expect(" set other k u8 255", "c6 nvs ESP_OK\n", 0);
    check(stored == 9 && store[1].value[0] == 0x80 && store[5].size == 15 && store[7].size == 3 && store[7].value[1] == 0xff,
          "set: bytes the C6 gets");

    expect(" get test n", "42\nc6 nvs ESP_OK\n", 0);
    expect(" get test neg", "-128\nc6 nvs ESP_OK\n", 0);
    expect(" get test big", "18446744073709551615\nc6 nvs ESP_OK\n", 0);
    expect(" get test min", "-9223372036854775808\nc6 nvs ESP_OK\n", 0);
    expect(" get test word", "-2\nc6 nvs ESP_OK\n", 0);
    expect(" get test s", "hello \\\\ world\\x01\nc6 nvs ESP_OK\n", 0);
    expect(" get test empty", "\nc6 nvs ESP_OK\n", 0);
    expect(" get test b", "00ff10\nc6 nvs ESP_OK\n", 0);
    expect(" get test missing", "c6 nvs ESP_ERR_NVS_NOT_FOUND\n", 1);

    expect(" list",
           "test n u32 4\ntest neg i8 1\ntest big u64 8\ntest min i64 8\ntest word i16 2\ntest s str 14\ntest empty str 0\ntest b blob 3\n"
           "other k u8 1\nc6 nvs ESP_OK\n",
           0);
    expect(" dump other", "other k u8 255\nc6 nvs ESP_OK\n", 0);
    expect("  dump",
           "test n u32 42\ntest neg i8 -128\ntest big u64 18446744073709551615\ntest min i64 -9223372036854775808\ntest word i16 -2\n"
           "test s str hello \\\\ world\\x01\ntest empty str \ntest b blob 00ff10\nother k u8 255\nc6 nvs ESP_OK\n",
           0);

    // Answers of a few entries and value pieces
    char line[300], hex[201] = "";
    for (int i = 0; i < 100; i++)
        snprintf(hex + 2 * i, 3, "%02x", (i * 37) & 0xff);
    snprintf(line, sizeof line, " set test long blob %s", hex);
    expect(line, "c6 nvs ESP_OK\n", 0);
    answer_limit = 30;
    exchanges = 0;
    char reply[300];
    snprintf(reply, sizeof reply, "%s\nc6 nvs ESP_OK\n", hex);
    expect(" get test long", reply, 0);
    check(exchanges == 4, "get: a value in pieces");
    expect(" dump other", "other k u8 255\nc6 nvs ESP_OK\n", 0);
    expect(" list test",
           "c6 nvs usage: list | get NAMESPACE KEY | dump [NAMESPACE] | set NAMESPACE KEY u8|i8|u16|i16|u32|i32|u64|i64|str|blob VALUE | "
           "erase NAMESPACE [KEY] | load [NAMESPACE]\n",
           1);
    answer_limit = C6_NVS_MESSAGE;

    expect(" set test x u8 256", "c6 nvs not a value of that type\n", 1);
    expect(" set test x u8 -1", "c6 nvs not a value of that type\n", 1);
    expect(" set test x i8 -129", "c6 nvs not a value of that type\n", 1);
    expect(" set test x u16 12a", "c6 nvs not a value of that type\n", 1);
    expect(" set test x u32", "c6 nvs not a value of that type\n", 1);
    expect(" set test x blob abc", "c6 nvs not hex\n", 1);
    expect(" set test x blob zz", "c6 nvs not hex\n", 1);
    check(find("test", "x") == NULL, "set: nothing sent for a refused value");
    run(" set test x float 1");
    check(strncmp(out, "c6 nvs usage: ", 14) == 0 && status == 1, "set: an unknown type");
    run(" get thisnamespaceistoolong k");
    check(strncmp(out, "c6 nvs usage: ", 14) == 0, "a name of 16 characters");
    run("");
    check(strncmp(out, "c6 nvs usage: ", 14) == 0, "no operation");
    run(" get test");
    check(strncmp(out, "c6 nvs usage: ", 14) == 0, "get without a key");

    expect(" erase test n", "c6 nvs ESP_OK\n", 0);
    expect(" get test n", "c6 nvs ESP_ERR_NVS_NOT_FOUND\n", 1);
    expect(" erase other", "c6 nvs ESP_OK\n", 0);
    expect(" dump other", "c6 nvs ESP_OK\n", 0);
    expect(" erase other", "c6 nvs ESP_ERR_NVS_NOT_FOUND\n", 1);

    // load: the dump format back, checked whole first
    static char dumped[32768], big_line[4200], big_hex[4001];
    expect(" set other k u8 9", "c6 nvs ESP_OK\n", 0);
    run(" dump");
    snprintf(dumped, sizeof dumped, "%s", out);
    int before = stored;
    feed(dumped);
    run(" load");
    check(status == 0 && strstr(out, " entries\nc6 nvs ESP_OK\n") != NULL && stored == before, "load: a whole dump back");
    run(" dump");
    check(strcmp(out, dumped) == 0, "load: dump, load, dump the same");
    feed("test a u8 7\r\ntest t str a\\\\b\\x01c\ntest e str \n\nc6 nvs ESP_OK\n");
    expect(" load test", "3 entries\nc6 nvs ESP_OK\n", 0);
    expect(" dump test", "test a u8 7\ntest t str a\\\\b\\x01c\ntest e str \nc6 nvs ESP_OK\n", 0);
    expect(" dump other", "other k u8 9\nc6 nvs ESP_OK\n", 0);
    before = stored;
    feed("test a u8 1\nother k u8 2\n");
    expect(" load test", "c6 nvs line 2: another namespace\n", 1);
    feed("test a u8 1\ntest b u8 300\n");
    expect(" load", "c6 nvs line 2: not a value of that type\n", 1);
    feed("test a u8 1\nc6 nvs ESP_ERR_TIMEOUT\n");
    expect(" load", "c6 nvs line 2: not a type set takes\n", 1);
    feed("test a u8 1\nnvs ESP_ERR_TIMEOUT\n");
    expect(" load", "c6 nvs line 2: not NAMESPACE KEY TYPE VALUE\n", 1);
    feed("test s str a\\qb\n");
    expect(" load", "c6 nvs line 1: a string with an unknown escape\n", 1);
    feed("test s str a\\x00b\n");
    expect(" load", "c6 nvs line 1: a string with \\x00\n", 1);
    feed("test b blob abc\n");
    expect(" load", "c6 nvs line 1: not hex\n", 1);
    check(stored == before, "load: nothing erased for a refused input");
    expect(" dump test", "test a u8 7\ntest t str a\\\\b\\x01c\ntest e str \nc6 nvs ESP_OK\n", 0);
    for (int i = 0; i < 2000; i++)
        snprintf(big_hex + 2 * i, 3, "%02x", (i * 53 + 7) & 0xff);
    snprintf(big_line, sizeof big_line, "big b blob %s\n", big_hex);
    feed(big_line);
    writes = 0;
    expect(" load big", "1 entries\nc6 nvs ESP_OK\n", 0);
    check(writes == 3, "load: a 2000-byte value in pieces");
    snprintf(big_line, sizeof big_line, "%s\nc6 nvs ESP_OK\n", big_hex);
    expect(" get big b", big_line, 0);
    feed("");
    expect(" load big", "0 entries\nc6 nvs ESP_OK\n", 0);
    expect(" dump big", "c6 nvs ESP_OK\n", 0);
    feed("");
    expect(" load nothere", "0 entries\nc6 nvs ESP_OK\n", 0);
    io.read = NULL;
    expect(" load", "c6 nvs load reads stdin: over SSH only\n", 1);
    io.read = read_input;
    io.protocol = C6_NVS_SINCE;
    expect(" load", "c6 nvs needs C6 protocol 7, the C6 firmware has 6: make c6-update\n", 1);
    expect(" get other k", "9\nc6 nvs ESP_OK\n", 0);
    io.protocol = C6_PROTOCOL;
    io.name = "nvs";
    expect(" get other k", "9\nnvs ESP_OK\n", 0);
    io.name = "c6 nvs";
    feed("");
    expect(" load", "0 entries\nc6 nvs ESP_OK\n", 0);
    check(stored == 0, "load: nothing in, nothing left");

    expect(" set test s str x", "c6 nvs ESP_OK\n", 0);
    malformed = true;
    expect(" get test s", "c6 nvs a malformed answer\n", 1);
    malformed = false;
    fail_with = TIMEOUT;
    expect(" list", "c6 nvs ESP_ERR_TIMEOUT\n", 1);
    fail_with = 0;
    io.protocol = 5;
    exchanges = 0;
    expect(" list", "c6 nvs needs C6 protocol 6, the C6 firmware has 5: make c6-update\n", 1);
    check(exchanges == 0, "an older C6 is never asked");

    const char* secret = "c6 nvs set  tab5 wifi  blob 0011";
    check(control_loggable("c6 nvs set tab5 wifi blob 0011") == 25, "loggable: c6 nvs set up to its type");
    check(control_loggable("c6 nvs set test s str a b c") == 21, "loggable: a string with spaces");
    check(control_loggable("c6 nvs set test k u8") == 20, "loggable: no value");
    check(control_loggable(secret) < (int)(strstr(secret, "0011") - secret), "loggable: more spaces cut earlier");
    check(control_loggable("wifi connect home password") == 12, "loggable: wifi connect");
    check(control_loggable("c6 nvs get tab5 wifi") == 20, "loggable: the rest whole");
    check(control_loggable("nvs set cfg config str {\"x\": 1}") == 22, "loggable: nvs set up to its type");
    check(control_nvs("nvs list") == 3 && control_nvs("c6 nvs") == 6 && control_nvs("nvsx") == 0 && control_nvs("c6 update 1 2") == 0,
          "the nvs commands");
    return failures != 0;
}
