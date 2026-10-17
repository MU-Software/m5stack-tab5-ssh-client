// SPDX-License-Identifier: GPL-3.0-or-later
#include "c6_nvs.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c6_protocol.h"

#define NAME_LENGTH 15
#define USAGE                                                                                                                      \
    "usage: list | get NAMESPACE KEY | dump [NAMESPACE] | set NAMESPACE KEY u8|i8|u16|i16|u32|i32|u64|i64|str|blob VALUE | erase " \
    "NAMESPACE [KEY]"

// The integers first
static const struct {
    uint8_t type;
    const char* name;
} types[] = {
    {C6_NVS_U8, "u8"},   {C6_NVS_I8, "i8"},   {C6_NVS_U16, "u16"}, {C6_NVS_I16, "i16"}, {C6_NVS_U32, "u32"},
    {C6_NVS_I32, "i32"}, {C6_NVS_U64, "u64"}, {C6_NVS_I64, "i64"}, {C6_NVS_STR, "str"}, {C6_NVS_BLOB, "blob"},
};
#define INTEGERS 8

typedef struct {
    const c6_nvs_io* io;
    // NULL until something fails
    const char* failure;
    uint8_t request[C6_NVS_MESSAGE], answer[C6_NVS_MESSAGE], entries[C6_NVS_MESSAGE];
    size_t answer_length;
} job;

static void put(const job* j, const char* text, size_t length) {
    if (length > 0) j->io->write(j->io->context, text, length);
}

static void say(const job* j, const char* format, ...) {
    char text[200];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(text, sizeof text, format, args);
    va_end(args);
    if (n > 0) put(j, text, (size_t)n < sizeof text ? (size_t)n : sizeof text - 1);
}

static int type_index(uint8_t type, const char* name) {
    for (int i = 0; i < (int)(sizeof types / sizeof types[0]); i++)
        if (name != NULL ? strcmp(types[i].name, name) == 0 : types[i].type == type) return i;
    return -1;
}

static const char* type_name(uint8_t type, char other[8]) {
    int i = type_index(type, NULL);
    if (i >= 0) return types[i].name;
    snprintf(other, 8, "0x%02x", type);
    return other;
}

static bool is_integer(uint8_t type) {
    int i = type_index(type, NULL);
    return i >= 0 && i < INTEGERS;
}

static uint32_t number_at(const uint8_t* bytes) {
    return bytes[0] | bytes[1] << 8 | bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static size_t put_number(uint8_t* out, size_t at, uint64_t n, int bytes) {
    for (int i = 0; i < bytes; i++)
        out[at + i] = (uint8_t)(n >> 8 * i);
    return at + bytes;
}

static size_t put_name(uint8_t* out, size_t at, const char* name) {
    out[at] = (uint8_t)strlen(name);
    memcpy(out + at + 1, name, out[at]);
    return at + 1 + out[at];
}

// The next word at *at into out, left alone when there is none or it does not fit
static bool word(const char** at, char* out, size_t size) {
    const char* start = *at + strspn(*at, " ");
    size_t n = strcspn(start, " ");
    if (n == 0 || n >= size) return false;
    memcpy(out, start, n);
    out[n] = 0;
    *at = start + n;
    return true;
}

static bool exchange(job* j, size_t length) {
    j->answer_length = sizeof j->answer;
    int err = j->io->exchange(j->request, length, j->answer, &j->answer_length);
    if (err != 0) j->failure = j->io->error_name(err);
    return err == 0;
}

// Text as it is, but for backslashes and control characters (\xHH)
static void write_text(const job* j, const uint8_t* text, size_t length) {
    char out[256] = "";
    size_t n = 0;
    for (size_t i = 0; i < length; i++) {
        if (n > sizeof out - 5) put(j, out, n), n = 0;
        if (text[i] == '\\')
            out[n++] = '\\', out[n++] = '\\';
        else if (text[i] < 0x20 || text[i] == 0x7f)
            n += (size_t)snprintf(out + n, 5, "\\x%02x", text[i]);
        else
            out[n++] = (char)text[i];
    }
    put(j, out, n);
}

static void write_hex(const job* j, const uint8_t* bytes, size_t length) {
    static const char digits[] = "0123456789abcdef";
    char out[256] = "";
    size_t n = 0;
    for (size_t i = 0; i < length; i++) {
        if (n == sizeof out) put(j, out, n), n = 0;
        out[n++] = digits[bytes[i] >> 4];
        out[n++] = digits[bytes[i] & 15];
    }
    put(j, out, n);
}

static void write_integer(const job* j, uint8_t type, const uint8_t* bytes) {
    int size = type & 0x0f;
    uint64_t n = 0;
    for (int i = size; i-- > 0;)
        n = n << 8 | bytes[i];
    if ((type & 0x10) && size > 0 && size < 8 && (n >> (size * 8 - 1)) != 0) n |= ~0ULL << size * 8;
    if (type & 0x10)
        say(j, "%" PRId64, (int64_t)n);
    else
        say(j, "%" PRIu64, n);
}

// One value as text, a line of its own, as "NAMESPACE KEY TYPE VALUE" when labelled
static bool value(job* j, const char* space, const char* key, bool labelled) {
    uint32_t offset = 0, size = 0;
    uint8_t type = 0;
    char other[8];
    bool started = false;
    for (; !started || offset < size; started = true) {
        size_t n = put_name(j->request, put_name(j->request, 1, space), key);
        j->request[0] = C6_NVS_GET;
        if (!exchange(j, put_number(j->request, n, offset, 4))) break;
        size_t chunk = j->answer_length >= 5 ? j->answer_length - 5 : 0;
        if (j->answer_length < 5 || (started && (j->answer[0] != type || number_at(j->answer + 1) != size)) ||
            chunk > number_at(j->answer + 1) - offset || (chunk == 0 && offset < number_at(j->answer + 1)) ||
            (is_integer(j->answer[0]) && chunk != (j->answer[0] & 0x0fu))) {
            j->failure = "a malformed answer";
            break;
        }
        type = j->answer[0];
        size = number_at(j->answer + 1);
        if (!started && labelled) say(j, "%s %s %s ", space, key, type_name(type, other));
        if (is_integer(type))
            write_integer(j, type, j->answer + 5);
        else if (type == C6_NVS_STR)
            write_text(j, j->answer + 5, offset + chunk == size && chunk > 0 ? chunk - 1 : chunk);
        else
            write_hex(j, j->answer + 5, chunk);
        offset += chunk;
    }
    if (started) put(j, "\n", 1);
    return j->failure == NULL;
}

// "NAMESPACE KEY TYPE SIZE", a string's without its terminator, or with values each value's line
static void entries(job* j, const char* only, bool values) {
    char other[8], space[NAME_LENGTH + 1], key[NAME_LENGTH + 1];
    for (unsigned index = 0;;) {
        j->request[0] = C6_NVS_LIST;
        if (!exchange(j, put_number(j->request, 1, index, 2)) || j->answer_length == 0) return;
        size_t length = j->answer_length;
        memcpy(j->entries, j->answer, length);
        for (size_t at = 0; at < length; index++) {
            const uint8_t* e = j->entries + at;
            if (at + 7 > length || e[1] > NAME_LENGTH || at + 7 + e[1] > length || e[2 + e[1]] > NAME_LENGTH ||
                at + 7 + e[1] + e[2 + e[1]] > length) {
                j->failure = "a malformed answer";
                return;
            }
            uint8_t type = e[0];
            snprintf(space, sizeof space, "%.*s", e[1], (const char*)e + 2);
            snprintf(key, sizeof key, "%.*s", e[2 + e[1]], (const char*)e + 3 + e[1]);
            uint32_t size = number_at(e + 3 + e[1] + e[2 + e[1]]);
            at += 7 + e[1] + e[2 + e[1]];
            if (only != NULL && strcmp(only, space) != 0) continue;
            if (!values)
                say(j, "%s %s %s %" PRIu32 "\n", space, key, type_name(type, other), type == C6_NVS_STR && size > 0 ? size - 1 : size);
            else if (!value(j, space, key, true))
                return;
        }
    }
}

static bool parse_integer(const char* text, uint8_t type, uint8_t* out) {
    int bits = (type & 0x0f) * 8;
    char* end;
    uint64_t n;
    text += strspn(text, " ");
    errno = 0;
    if (type & 0x10) {
        long long v = strtoll(text, &end, 10);
        if (bits < 64 && (v < -(1LL << (bits - 1)) || v >= 1LL << (bits - 1))) return false;
        n = (uint64_t)v;
    } else {
        n = strtoull(text, &end, 10);
        if (text[0] == '-' || (bits < 64 && n >> bits != 0)) return false;
    }
    if (errno != 0 || end == text || *end != 0) return false;
    put_number(out, 0, n, bits / 8);
    return true;
}

static int hex_digit(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// A string as it is, a blob in hex
static void set(job* j, const char* space, const char* key, const char* type_text, const char* text) {
    int i = type_index(0, type_text);
    if (i < 0) {
        j->failure = USAGE;
        return;
    }
    uint8_t type = types[i].type;
    size_t n = put_name(j->request, put_name(j->request, 1, space), key), length = strlen(text);
    j->request[0] = C6_NVS_SET;
    j->request[n++] = type;
    if (type == C6_NVS_BLOB) text += strspn(text, " "), length = strlen(text);
    if (is_integer(type)) {
        if (!parse_integer(text, type, j->request + n)) j->failure = "not a value of that type";
        n += type & 0x0f;
    } else if ((type == C6_NVS_STR ? length : length / 2) > sizeof j->request - n) {
        j->failure = "too long";
    } else if (type == C6_NVS_STR) {
        memcpy(j->request + n, text, length);
        n += length;
    } else {
        for (size_t k = 0; k < length && j->failure == NULL; k += 2, n++)
            if (k + 1 >= length || hex_digit(text[k]) < 0 || hex_digit(text[k + 1]) < 0)
                j->failure = "not hex";
            else
                j->request[n] = (uint8_t)(hex_digit(text[k]) << 4 | hex_digit(text[k + 1]));
    }
    if (j->failure == NULL) exchange(j, n);
}

int c6_nvs_command(const char* arguments, const c6_nvs_io* io) {
    char words[4][NAME_LENGTH + 1] = {{0}};
    const char* at = arguments;
    int count = 0;
    if (io->protocol < C6_NVS_SINCE) {
        char text[96];
        int n = snprintf(text, sizeof text, "c6 nvs needs C6 protocol %d, the C6 firmware has %u: make c6-update\n", C6_NVS_SINCE, io->protocol);
        io->write(io->context, text, (size_t)n);
        return 1;
    }
    job* j = calloc(1, sizeof *j);
    if (j == NULL) {
        io->write(io->context, "c6 nvs out of memory\n", 21);
        return 1;
    }
    j->io = io;
    while (count < 4 && word(&at, words[count], sizeof words[count]))
        count++;
    const char *operation = words[0], *space = words[1], *key = words[2];
    bool rest = at[strspn(at, " ")] != 0;
    if (count == 1 && !rest && strcmp(operation, "list") == 0) {
        entries(j, NULL, false);
    } else if ((count == 1 || count == 2) && !rest && strcmp(operation, "dump") == 0) {
        entries(j, count == 2 ? space : NULL, true);
    } else if (count == 3 && !rest && strcmp(operation, "get") == 0) {
        value(j, space, key, false);
    } else if (count == 4 && strcmp(operation, "set") == 0) {
        set(j, space, key, words[3], at[0] == ' ' ? at + 1 : at);
    } else if ((count == 2 || count == 3) && !rest && strcmp(operation, "erase") == 0) {
        size_t n = put_name(j->request, put_name(j->request, 1, space), key);
        j->request[0] = C6_NVS_ERASE;
        exchange(j, n);
    } else {
        j->failure = USAGE;
    }
    bool ok = j->failure == NULL;
    say(j, "c6 nvs %s\n", ok ? io->error_name(0) : j->failure);
    for (volatile uint8_t* p = (volatile uint8_t*)j; p < (volatile uint8_t*)(j + 1); p++)
        *p = 0;
    free(j);
    return !ok;
}
