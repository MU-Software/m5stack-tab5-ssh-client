// SPDX-License-Identifier: GPL-3.0-or-later
#include "modem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Kept by the firmware: CURCCFG and CBATCHK are AUTO_SAVE switches that send nothing to the network; CEDRXS=0,4 costs
// nothing while eDRX is off (research X2). CNMP, CMNB, CBANDCFG and CPSMS writes can mean a new attach, a back-off risk.
// The rest are reads; CPSI and CEREG prefixes without the space, as the dial's checks have always matched them
const modem_command modem_commands[MODEM_ITEMS] = {
    [MODEM_QUALCOMM_URC] = {"qualcomm_urc", "AT+CURCCFG?", "+CURCCFG: \"QUALCOMM\",", "AT+CURCCFG=\"QUALCOMM\",1"},
    [MODEM_BATTERY_CHECK] = {"battery_check", "AT+CBATCHK?", "+CBATCHK: ", "AT+CBATCHK=1"},
    [MODEM_EDRX] = {"edrx", "AT+CEDRXS?", "+CEDRXS: 4,", "AT+CEDRXS=0,4"},
    [MODEM_EDRX_NETWORK] = {"edrx_network", "AT+CEDRXRDP", "+CEDRXRDP: ", NULL},
    [MODEM_PSM] = {"psm", "AT+CPSMS?", "+CPSMS: ", NULL},
    [MODEM_MODE] = {"mode", "AT+CNMP?", "+CNMP: ", NULL},
    [MODEM_CATEGORY] = {"category", "AT+CMNB?", "+CMNB: ", NULL},
    [MODEM_BANDS] = {"bands", "AT+CBANDCFG?", "+CBANDCFG: \"CAT-M\",", NULL},
    [MODEM_REVISION] = {"revision", "AT+CGMR", "Revision:", NULL},
    [MODEM_REJECTS] = {"rejects", "AT+CTRJ", "+CTRJ:", NULL},
    [MODEM_BACKOFF] = {"backoff", "AT+CNVR=1,\"/nv/item_files/modem/nas/mm_backoff_remaining_info\"", "+CNVR:", NULL},
    [MODEM_REGISTRATION] = {"registration", "AT+CEREG?", "+CEREG:", NULL},
    [MODEM_SYSTEM] = {"system", "AT+CPSI?", "+CPSI:", NULL},
    [MODEM_BEARER] = {"bearer", "AT+CGACT?", "+CGACT: 1,", NULL},
    [MODEM_SIGNAL] = {"signal", "AT+CSQ", "+CSQ: ", NULL},
    [MODEM_OPERATOR] = {"operator", "AT+COPS?", "+COPS: ", NULL},
    [MODEM_SIM] = {"sim", "AT+CPIN?", "+CPIN: ", NULL},
    // The reply is the ICCID alone, which starts with 89 (telecom)
    [MODEM_ICCID] = {"iccid", "AT+CCID", "89", NULL},
    // "<alpha>","<number>",<type> for each number the SIM keeps, often none for a data SIM
    [MODEM_NUMBER] = {"number", "AT+CNUM", "+CNUM: ", NULL},
    [MODEM_CONTEXT] = {"context", "AT+CGDCONT?", "+CGDCONT: 1,", NULL},
    [MODEM_ADDRESS] = {"address", "AT+CGPADDR", "+CGPADDR: 1,", NULL},
};

// Where the number of a +CNUM line or value starts, NULL for an empty one or another line
static const char* number_at(const char* line) {
    if (strncmp(line, "+CNUM: ", 7) == 0) line += 7;
    const char* number = line[0] == '"' ? strstr(line + 1, "\",\"") : NULL;
    if (number == NULL) return NULL;
    number += 3;
    size_t n = strspn(number, "+0123456789");
    return n > 0 && number[n] == '"' ? number : NULL;
}

void modem_mask(const char* line, char* out, size_t size) {
    size_t n = strlen(line);
    const char* number = number_at(line);
    if (strncmp(line, "89", 2) == 0 && n >= 18 && n <= 22 && strspn(line, "0123456789abcdefABCDEF") == n) {
        snprintf(out, size, "%.6s…%s", line, line + n - 4);
    } else if (number != NULL) {
        n = strspn(number, "+0123456789");
        snprintf(out, size, "%.*s…%s", (int)(number - line), line, number + (n > 4 ? n - 4 : 0));
    } else {
        snprintf(out, size, "%s", line);
    }
}

bool modem_kept(const char* line, const char* prefix, const char* reply) {
    return strncmp(line, prefix, strlen(prefix)) == 0 && number_at(reply) == NULL;
}

void modem_value(const char* line, const char* prefix, char* value, size_t size) {
    size_t n = strlen(prefix);
    const char* rest = "";
    // The ICCID's prefix is part of it
    if (strncmp(line, prefix, n) == 0) rest = strcmp(prefix, "89") == 0 ? line : line + n;
    rest += strspn(rest, " ");
    // The number whole for the screen; only logs mask it
    if (number_at(rest) != NULL)
        snprintf(value, size, "%s", rest);
    else
        modem_mask(rest, value, size);
}

// The NV item's 32 hex digits, NULL without them
static const char* backoff_digits(const char* value) {
    for (const char* p = value; *p; p++) {
        size_t n = strspn(p, "0123456789abcdefABCDEF");
        if (n == 32) return p;
        p += n > 0 ? n - 1 : 0;
    }
    return NULL;
}

// The item's first 16 hex digits are the T3346 back-off and timer 20 left
bool modem_backoff_clear(const char* value) {
    const char* digits = backoff_digits(value);
    return digits != NULL && strspn(digits, "0") >= 16;
}

// Field n (from 0) of a comma-separated reply, without quotes; "?" when there is none
static const char* field(const char* value, int n, char* out, size_t size) {
    for (; n > 0 && value != NULL; n--)
        value = strchr(value, ',') != NULL ? strchr(value, ',') + 1 : NULL;
    if (value == NULL || *value == 0 || *value == ',') return snprintf(out, size, "?"), out;
    if (*value == '"') value++;
    size_t length = strcspn(value, ",\"");
    snprintf(out, size, "%.*s", (int)length, value);
    return out;
}

// "0001" (quotes optional) as the eDRX cycle in seconds, NULL for anything else
static const char* cycle(const char* bits) {
    static const char* const seconds[] = {"5.12",   "10.24",  "20.48",  "40.96",  "61.44",   "81.92",   "102.4",   "122.88",
                                          "143.36", "163.84", "327.68", "655.36", "1310.72", "2621.44", "5242.88", "10485.76"};
    char digits[5];
    if (*bits == '"') bits++;
    if (strspn(bits, "01") != 4) return NULL;
    memcpy(digits, bits, 4);
    digits[4] = 0;
    return seconds[strtol(digits, NULL, 2)];
}

// +CEDRXRDP: <AcT-type>,<requested>,<network-provided>,<paging time window>, or 0 while eDRX is unused; the
// network-provided cycle, NULL without one
static const char* granted(const char* value) {
    const char* second = strchr(value, ',');
    const char* third = second != NULL ? strchr(second + 1, ',') : NULL;
    return strncmp(value, "4,", 2) == 0 && third != NULL ? cycle(third + 1) : NULL;
}

// CEDRXS? keeps showing the value asked for once eDRX is off, so only CEDRXRDP tells whether it is in use
bool modem_wanted(const modem_snapshot* s, modem_item item) {
    if (modem_commands[item].write == NULL) return false;
    if (item == MODEM_EDRX) return s->read_at[MODEM_EDRX_NETWORK] != 0 && granted(s->values[MODEM_EDRX_NETWORK]) != NULL;
    return s->read_at[item] != 0 && strcmp(s->values[item], "0") == 0;
}

static const char* named(const char* value, const char* const* names, int count) {
    char* end;
    long n = strtol(value, &end, 10);
    return *value != 0 && *end == 0 && n >= 0 && n < count && names[n] != NULL ? names[n] : value[0] != 0 ? value : "모름";
}

static const struct {
    const char* name;
    modem_item item;
} shown[] = {
    {"qualcomm_urc", MODEM_QUALCOMM_URC},
    {"battery_check", MODEM_BATTERY_CHECK},
    {"edrx", MODEM_EDRX_NETWORK},
    {"psm", MODEM_PSM},
    {"mode", MODEM_MODE},
    {"bands", MODEM_BANDS},
    {"revision", MODEM_REVISION},
    {"registration", MODEM_REGISTRATION},
    {"operator", MODEM_OPERATOR},
    {"cell", MODEM_SYSTEM},
    {"signal", MODEM_SYSTEM},
    {"bearer", MODEM_BEARER},
    {"sim", MODEM_SIM},
    {"number", MODEM_NUMBER},
    {"backoff", MODEM_BACKOFF},
};

modem_item modem_text(const modem_snapshot* s, const char* name, char* out, size_t size) {
    static const char* const modes[52] = {[2] = "자동", [13] = "GSM만", [38] = "LTE만", [51] = "GSM·LTE"};
    static const char* const categories[] = {NULL, "CAT-M", "NB-IoT", "CAT-M·NB-IoT"};
    static const char* const registrations[] = {"등록 안 함", "등록됨 (홈)", "찾는 중", "거부됨", "모름", "등록됨 (로밍)"};
    char a[40], b[40], c[40], d[40];
    size_t i = 0;
    while (i < sizeof shown / sizeof shown[0] && strcmp(shown[i].name, name) != 0)
        i++;
    if (i == sizeof shown / sizeof shown[0]) return MODEM_ITEMS;
    modem_item item = shown[i].item;
    const char *v = s->values[item], *system = s->values[MODEM_SYSTEM];
    if (s->read_at[item] == 0) {
        snprintf(out, size, "?");
    } else if (item == MODEM_QUALCOMM_URC || item == MODEM_BATTERY_CHECK) {
        snprintf(out, size, "%s", strcmp(v, "1") == 0 ? "켬" : strcmp(v, "0") == 0 ? "끔" : "모름");
    } else if (item == MODEM_EDRX_NETWORK) {
        const char *given = granted(v), *asked = cycle(s->values[MODEM_EDRX]);
        if (given != NULL)
            snprintf(out, size, "켜짐 · 주기 %s초", given);
        else if (v[0] == 0)
            snprintf(out, size, "모름");
        else if (asked != NULL)
            snprintf(out, size, "꺼짐 · %s초로 요청해 둠", asked);
        else
            snprintf(out, size, "꺼짐");
    } else if (item == MODEM_PSM) {
        bool alone = v[0] != 0 && (v[1] == 0 || v[1] == ',');
        if (alone && v[0] == '0')
            snprintf(out, size, "끔");
        else if (alone && v[0] == '1')
            snprintf(out, size, "켬 (%s)", v);
        else
            snprintf(out, size, "%s", v[0] != 0 ? v : "모름");
    } else if (item == MODEM_MODE) {
        snprintf(out, size, "%s · %s", named(v, modes, 52), named(s->values[MODEM_CATEGORY], categories, 4));
    } else if (item == MODEM_REGISTRATION) {
        snprintf(out, size, "%s", named(field(v, 1, a, sizeof a), registrations, 6));
    } else if (item == MODEM_OPERATOR) {
        snprintf(out, size, "%s · %s", field(v, 2, a, sizeof a), s->read_at[MODEM_SYSTEM] != 0 ? field(system, 2, b, sizeof b) : "?");
    } else if (item == MODEM_SYSTEM && strncmp(system, "NO SERVICE", 10) == 0) {
        snprintf(out, size, "서비스 없음");
    } else if (strcmp(name, "cell") == 0) {
        snprintf(out, size, "%s · %s · TAC %s · 셀 %s", field(system, 0, a, sizeof a), field(system, 6, b, sizeof b), field(system, 3, c, sizeof c),
                 field(system, 4, d, sizeof d));
    } else if (strcmp(name, "signal") == 0) {
        // SINR = 2 * RSSNR - 20 (AT manual, AT+CPSI)
        char* end;
        long rssnr = strtol(field(system, 13, d, sizeof d), &end, 10);
        if (*end == 0) snprintf(d, sizeof d, "%ld", 2 * rssnr - 20);
        snprintf(out, size, "RSRP %s · RSRQ %s · SINR %s · CSQ %s", field(system, 11, a, sizeof a), field(system, 10, b, sizeof b), d,
                 s->read_at[MODEM_SIGNAL] != 0 ? field(s->values[MODEM_SIGNAL], 0, c, sizeof c) : "?");
    } else if (item == MODEM_BEARER) {
        snprintf(out, size, "%s · %s · APN %s",
                 strcmp(v, "1") == 0   ? "켜짐"
                 : strcmp(v, "0") == 0 ? "꺼짐"
                                       : "모름",
                 s->read_at[MODEM_ADDRESS] != 0 ? field(s->values[MODEM_ADDRESS], 0, a, sizeof a) : "?",
                 s->read_at[MODEM_CONTEXT] != 0 ? field(s->values[MODEM_CONTEXT], 1, b, sizeof b) : "?");
    } else if (item == MODEM_SIM) {
        snprintf(out, size, "%s · %s", v[0] != 0 ? v : "모름", s->values[MODEM_ICCID][0] != 0 ? s->values[MODEM_ICCID] : "?");
    } else if (item == MODEM_NUMBER) {
        const char* number = number_at(v);
        if (number != NULL)
            snprintf(out, size, "%.*s", (int)strspn(number, "+0123456789"), number);
        else
            snprintf(out, size, "없음");
    } else if (item == MODEM_BACKOFF) {
        const char* digits = backoff_digits(v);
        if (modem_backoff_clear(v) && strcmp(s->values[MODEM_REJECTS], "0") == 0)
            snprintf(out, size, "없음");
        else
            snprintf(out, size, "있을 수 있음 · CTRJ %s · NV %.16s", s->values[MODEM_REJECTS], digits != NULL ? digits : "?");
    } else {
        snprintf(out, size, "%s", v[0] != 0 ? v : "모름");
    }
    return item;
}
