// SPDX-License-Identifier: GPL-3.0-or-later
#include "modem.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

// Each reply line as at() keeps it, the line that starts with the item's prefix or none (NULL: the modem answered ERROR)
static modem_snapshot parsed(const char* const replies[MODEM_ITEMS]) {
    modem_snapshot s = {0};
    for (int i = 0; i < MODEM_ITEMS; i++) {
        modem_value(replies[i] != NULL ? replies[i] : "", modem_commands[i].prefix, s.values[i], sizeof s.values[i]);
        s.read_at[i] = 1;
    }
    return s;
}

static const char* shown(const modem_snapshot* s, const char* name) {
    static char text[96];
    return modem_text(s, name, text, sizeof text) != MODEM_ITEMS ? text : "(none)";
}

int main(void) {
    // As the research module answered (CATM.md, stamp-catm-wiring, catm-ctrj-and-backoff) and the manual's examples
    const char* const bench[MODEM_ITEMS] = {
        [MODEM_QUALCOMM_URC] = "+CURCCFG: \"QUALCOMM\",1",
        [MODEM_BATTERY_CHECK] = "+CBATCHK: 1",
        [MODEM_EDRX] = "+CEDRXS: 4,\"0001\"",
        [MODEM_EDRX_NETWORK] = "+CEDRXRDP: 0",
        [MODEM_PSM] = "+CPSMS: 0,,,\"01100000\",\"00000000\"",
        [MODEM_MODE] = "+CNMP: 2",
        [MODEM_CATEGORY] = "+CMNB: 1",
        [MODEM_BANDS] = "+CBANDCFG: \"CAT-M\",1,2,3,4,5,8,12,13,14,18,19,20,25,26,27,28,66,85",
        [MODEM_REVISION] = "Revision:1951B17SIM7080",
        [MODEM_REJECTS] = "+CTRJ: 0",
        [MODEM_BACKOFF] = "+CNVR: 16,00000000000000000000000000000000",
        [MODEM_REGISTRATION] = "+CEREG: 0,1",
        [MODEM_SYSTEM] = "+CPSI: LTE CAT-M1,Online,450-06,0x420F,14841609,82,EUTRAN-BAND5,2506,0,0,-7,-102,-75,13",
        [MODEM_BEARER] = "+CGACT: 1,1",
        [MODEM_SIGNAL] = "+CSQ: 18,99",
        [MODEM_OPERATOR] = "+COPS: 0,0,\"LG U+\",7",
        [MODEM_SIM] = "+CPIN: READY",
        [MODEM_ICCID] = "89820612345678901234",
        [MODEM_NUMBER] = "+CNUM: \"\",\"+821012345678\",145",
        [MODEM_CONTEXT] = "+CGDCONT: 1,\"IP\",\"internet.lguplus.co.kr\",\"0.0.0.0\",0,0,0,0",
        [MODEM_ADDRESS] = "+CGPADDR: 1,\"10.53.105.62\"",
    };
    modem_snapshot s = parsed(bench);
    check(strcmp(s.values[MODEM_QUALCOMM_URC], "1") == 0 && strcmp(s.values[MODEM_EDRX], "\"0001\"") == 0 &&
              strcmp(s.values[MODEM_REJECTS], "0") == 0 && strncmp(s.values[MODEM_SYSTEM], "LTE CAT-M1,Online,450-", 22) == 0 &&
              strcmp(s.values[MODEM_BEARER], "1") == 0,
          "values after their prefixes and spaces, as the dial's checks read them");
    check(strcmp(s.values[MODEM_ICCID], "898206…1234") == 0, "the ICCID kept masked");
    char masked[64];
    modem_mask("89820612345678901234", masked, sizeof masked);
    check(strcmp(masked, "898206…1234") == 0, "an ICCID line masked for the log");
    modem_mask("8982061234567890123F", masked, sizeof masked);
    check(strcmp(masked, "898206…123F") == 0, "with a hex digit too");
    modem_mask("+CPIN: READY", masked, sizeof masked);
    check(strcmp(masked, "+CPIN: READY") == 0, "other lines as they are");
    check(modem_backoff_clear(s.values[MODEM_BACKOFF]) && !modem_backoff_clear("16,00000000000000010000000000000000") &&
              !modem_backoff_clear("16,00000000253E5A000000000000000000") && !modem_backoff_clear(""),
          "the NV back-off: zero front half only, the stale 253E5A00 of the research not");
    check(modem_address_moved("10.37.187.15", "10.33.61.97") && !modem_address_moved("10.33.61.97", "10.33.61.97") &&
              !modem_address_moved("", "10.33.61.97") && !modem_address_moved("0.0.0.0", "10.33.61.97") &&
              !modem_address_moved("10.37.187.15,fe80::1", "10.33.61.97"),
          "a new bearer: another address only, none or 0.0.0.0 unknown");
    check(strcmp(shown(&s, "qualcomm_urc"), "켬") == 0 && strcmp(shown(&s, "battery_check"), "켬") == 0, "switches as 켬");
    check(strcmp(shown(&s, "edrx"), "꺼짐 · 10.24초로 요청해 둠") == 0, "eDRX remembered but not granted");
    check(!modem_wanted(&s, MODEM_QUALCOMM_URC) && !modem_wanted(&s, MODEM_BATTERY_CHECK) && !modem_wanted(&s, MODEM_EDRX),
          "the bench module needs no write: CEDRXS? alone does not call for one");
    check(strcmp(shown(&s, "psm"), "끔") == 0 && strcmp(shown(&s, "mode"), "자동 · CAT-M") == 0, "PSM, CNMP and CMNB");
    check(strcmp(shown(&s, "bands"), "1,2,3,4,5,8,12,13,14,18,19,20,25,26,27,28,66,85") == 0 && strcmp(shown(&s, "revision"), "1951B17SIM7080") == 0,
          "bands and revision");
    check(strcmp(shown(&s, "registration"), "등록됨 (홈)") == 0 && strcmp(shown(&s, "operator"), "LG U+ · 450-06") == 0, "registration and operator");
    check(strcmp(shown(&s, "cell"), "LTE CAT-M1 · EUTRAN-BAND5 · TAC 0x420F · 셀 14841609") == 0, "the cell from CPSI");
    check(strcmp(shown(&s, "signal"), "RSRP -102 · RSRQ -7 · SINR 6 · CSQ 18") == 0, "signal, SINR = 2 RSSNR - 20");
    check(strcmp(shown(&s, "bearer"), "켜짐 · 10.53.105.62 · APN internet.lguplus.co.kr") == 0, "the PDP context");
    check(strcmp(shown(&s, "sim"), "READY · 898206…1234") == 0, "SIM with the ICCID masked");
    check(strcmp(shown(&s, "backoff"), "없음") == 0, "no back-off in sight");
    check(strcmp(shown(&s, "number"), "+821012345678") == 0, "the number in full on the screen");
    modem_mask("+CNUM: \"\",\"+821012345678\",145", masked, sizeof masked);
    check(strcmp(masked, "+CNUM: \"\",\"…5678\",145") == 0, "a +CNUM line masked for the log");
    modem_mask(s.values[MODEM_NUMBER], masked, sizeof masked);
    check(strcmp(masked, "\"\",\"…5678\",145") == 0, "its value too, for modem status");
    modem_mask(s.values[MODEM_CONTEXT], masked, sizeof masked);
    check(strcmp(masked, s.values[MODEM_CONTEXT]) == 0, "the context's quoted fields are no number");
    modem_mask(s.values[MODEM_PSM], masked, sizeof masked);
    check(strcmp(masked, s.values[MODEM_PSM]) == 0, "nor PSM's timers");
    check(modem_text(&s, "signal", masked, sizeof masked) == MODEM_SYSTEM && modem_text(&s, "edrx", masked, sizeof masked) == MODEM_EDRX_NETWORK &&
              modem_text(&s, "category", masked, sizeof masked) == MODEM_ITEMS,
          "each name with the item its age goes by; only the page's names");

    const char* const other[MODEM_ITEMS] = {
        [MODEM_QUALCOMM_URC] = "+CURCCFG: \"SYS\",1", // another type's line: none for QUALCOMM
        [MODEM_BATTERY_CHECK] = "+CBATCHK: 0",
        [MODEM_EDRX] = NULL,
        [MODEM_EDRX_NETWORK] = "+CEDRXRDP: 4,\"0010\",\"0001\",\"0011\"",
        [MODEM_PSM] = "+CPSMS: 1,,,\"00100001\",\"00000011\"",
        [MODEM_MODE] = "+CNMP: 38",
        [MODEM_CATEGORY] = "+CMNB: 7",
        [MODEM_BANDS] = "+CBANDCFG: \"NB-IOT\",1,2,3",
        [MODEM_REJECTS] = "+CTRJ: 5911",
        [MODEM_BACKOFF] = "+CNVR: 16,00000000253E5A000000000000000000",
        [MODEM_REGISTRATION] = "+CEREG: 0,2",
        [MODEM_SYSTEM] = "+CPSI: NO SERVICE,Online",
        [MODEM_BEARER] = "+CGACT: 1,0",
        [MODEM_OPERATOR] = "+COPS: 0",
    };
    s = parsed(other);
    check(strcmp(shown(&s, "number"), "없음") == 0, "no number after ERROR");
    check(strcmp(shown(&s, "qualcomm_urc"), "모름") == 0 && strcmp(shown(&s, "battery_check"), "끔") == 0, "a missing line is unknown, 0 is 끔");
    check(strcmp(shown(&s, "edrx"), "켜짐 · 주기 10.24초") == 0, "eDRX granted by the network");
    check(!modem_wanted(&s, MODEM_QUALCOMM_URC) && modem_wanted(&s, MODEM_BATTERY_CHECK) && modem_wanted(&s, MODEM_EDRX) &&
              !modem_wanted(&s, MODEM_PSM) && !modem_wanted(&s, MODEM_MODE),
          "writes for a switch at 0 and granted eDRX only, none for an unknown switch");
    check(strcmp(shown(&s, "psm"), "켬 (1,,,\"00100001\",\"00000011\")") == 0 && strcmp(shown(&s, "mode"), "LTE만 · 7") == 0 &&
              strcmp(shown(&s, "bands"), "모름") == 0,
          "PSM on, an unknown CMNB, no CAT-M bands line");
    check(strcmp(shown(&s, "registration"), "찾는 중") == 0 && strcmp(shown(&s, "cell"), "서비스 없음") == 0 &&
              strcmp(shown(&s, "operator"), "? · ?") == 0,
          "searching, no service");
    check(strcmp(shown(&s, "backoff"), "있을 수 있음 · CTRJ 5911 · NV 00000000253E5A00") == 0, "a back-off shown with what showed it");
    check(strcmp(shown(&s, "bearer"), "꺼짐 · ? · APN ?") == 0 && strcmp(shown(&s, "sim"), "모름 · ?") == 0, "no bearer, no SIM reply");
    s.values[MODEM_QUALCOMM_URC][0] = '0', s.values[MODEM_QUALCOMM_URC][1] = 0;
    snprintf(s.values[MODEM_EDRX_NETWORK], sizeof s.values[MODEM_EDRX_NETWORK], "4");
    check(modem_wanted(&s, MODEM_QUALCOMM_URC) && !modem_wanted(&s, MODEM_EDRX), "QUALCOMM at 0; CAT-M without a granted cycle is no grant");
    snprintf(s.values[MODEM_EDRX_NETWORK], sizeof s.values[MODEM_EDRX_NETWORK], "5,\"0010\",\"0001\",\"0011\"");
    check(!modem_wanted(&s, MODEM_EDRX), "eDRX granted for NB-IoT is not CAT-M's");
    snprintf(s.values[MODEM_EDRX_NETWORK], sizeof s.values[MODEM_EDRX_NETWORK], "%s", "");
    check(strcmp(shown(&s, "edrx"), "모름") == 0, "eDRX unknown without a CEDRXRDP line");

    const char* const numbers[][3] = {
        {"+CNUM: \"\",\"\",129", NULL, "없음"},
        {"+CNUM: \"\",\"01012345678\",129", NULL, "01012345678"},
        {"+CNUM: \"\",\"\",129", "+CNUM: \"Line 2\",\"01087654321\",129", "01087654321"},
        {"+CNUM: \"\",\"01012345678\",129", "+CNUM: \"\",\"\",129", "01012345678"},
        {"+CNUM: \"\",\"01012345678\",129", "+CNUM: \"Fax\",\"01087654321\",129", "01012345678"},
    };
    for (size_t i = 0; i < sizeof numbers / sizeof numbers[0]; i++) {
        char reply[128] = "";
        for (int k = 0; k < 3; k++) {
            const char* line = k == 2 ? "OK" : numbers[i][k];
            if (line != NULL && modem_kept(line, modem_commands[MODEM_NUMBER].prefix, reply)) snprintf(reply, sizeof reply, "%s", line);
        }
        modem_value(reply, modem_commands[MODEM_NUMBER].prefix, s.values[MODEM_NUMBER], sizeof s.values[MODEM_NUMBER]);
        check(strcmp(shown(&s, "number"), numbers[i][2]) == 0, numbers[i][2]);
    }
    char reply[128] = "+CGACT: 1,1";
    check(modem_kept("+CGACT: 1,0", "+CGACT: 1,", reply) && !modem_kept("+CGACT: 2,1", "+CGACT: 1,", reply),
          "other replies: the last line with the prefix");

    modem_snapshot unread = {0};
    unread.values[MODEM_BATTERY_CHECK][0] = '0';
    check(strcmp(shown(&unread, "battery_check"), "?") == 0 && !modem_wanted(&unread, MODEM_BATTERY_CHECK), "? and no write before any read");

    const char* const kept[] = {"AT+CURCCFG=\"QUALCOMM\",1", "AT+CBATCHK=1", "AT+CEDRXS=0,4"};
    size_t writes = 0, listed = 0;
    for (int i = 0; i < MODEM_ITEMS; i++) {
        check(strchr(modem_commands[i].read, '=') == NULL || i == MODEM_BACKOFF, modem_commands[i].read);
        if (modem_commands[i].write == NULL) continue;
        writes++;
        for (size_t k = 0; k < sizeof kept / sizeof kept[0]; k++)
            listed += strcmp(modem_commands[i].write, kept[k]) == 0;
    }
    check(writes == 3 && listed == 3, "the writes are exactly CURCCFG QUALCOMM 1, CBATCHK 1 and CEDRXS 0,4");
    return failures != 0;
}
