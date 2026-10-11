// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <string.h>

#include "hangul.h"

static int failures;

// Keys typed one by one ('<' is Backspace), then the composition flushed
static void check(const char* keys, const char* expected) {
    hangul h = HANGUL_EMPTY;
    char text[256] = "";
    size_t n = 0, written;
    for (const char* k = keys; *k; k++) {
        if (*k == '<')
            hangul_backspace(&h);
        else if (hangul_key(&h, *k, text + n, &written))
            n += written;
    }
    n += hangul_flush(&h, text + n);
    text[n] = 0;
    if (strcmp(text, expected) != 0) {
        printf("FAIL %s: %s, expected %s\n", keys, text, expected);
        failures++;
    }
}

int main(void) {
    check("gksrmf", "한글");
    check("dkssud", "안녕");
    check("qkfrdms", "밝은");
    check("dkfrl", "알기");
    check("dkfk", "아라");
    check("rkqt", "값");
    check("rkqtdl", "값이");
    check("dhk", "와");
    check("dml", "의");
    check("Rk", "까");
    check("dO", "얘");
    check("rr", "ㄱㄱ");
    check("kk", "ㅏㅏ");
    check("hk", "ㅘ");
    check("gks<", "하");
    check("gks<<", "ㅎ");
    check("gks<<<", "");
    check("rkqt<", "갑");
    check("dhk<", "오");
    check("ekfr<<", "다");
    printf("%s\n", failures ? "hangul: FAILED" : "hangul: ok");
    return failures != 0;
}
