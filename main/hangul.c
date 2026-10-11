// SPDX-License-Identifier: GPL-3.0-or-later
#include "hangul.h"

#include <string.h>

// QWERTY keys of the initial consonants and vowels, in Unicode order; '_' for vowels typed as two keys
static const char initials[] = "rRseEfaqQtTdwWczxvg";
static const char medials[] = "koiOjpuPh___yn___bm_l";
// The final of each initial consonant, 0 for those that cannot end a syllable
static const uint8_t finals[19] = {1, 2, 4, 7, 0, 8, 16, 17, 0, 19, 20, 21, 22, 0, 23, 24, 25, 26, 27};
// Pairs typed as two jamo: {first, second, together}; finals: second is an initial
static const uint8_t double_finals[][3] = {{1, 9, 3},  {4, 12, 5},  {4, 18, 6},  {8, 0, 9},   {8, 6, 10}, {8, 7, 11},
                                           {8, 9, 12}, {8, 16, 13}, {8, 17, 14}, {8, 18, 15}, {17, 9, 18}};
static const uint8_t double_medials[][3] = {{8, 0, 9}, {8, 1, 10}, {8, 20, 11}, {13, 4, 14}, {13, 5, 15}, {13, 20, 16}, {18, 20, 19}};
// Compatibility jamo of the initial consonants, for one typed alone
static const uint16_t lone[19] = {0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141, 0x3142, 0x3143, 0x3145,
                                  0x3146, 0x3147, 0x3148, 0x3149, 0x314a, 0x314b, 0x314c, 0x314d, 0x314e};

static int find(const char* keys, char letter) {
    const char* p = strchr(keys, letter);
    if (p == NULL && letter >= 'A' && letter <= 'Z') p = strchr(keys, letter - 'A' + 'a');
    return p != NULL && letter != '_' ? (int)(p - keys) : -1;
}

static int pair(const uint8_t (*pairs)[3], size_t count, int first, int second) {
    for (size_t i = 0; i < count; i++)
        if (pairs[i][0] == first && pairs[i][1] == second) return pairs[i][2];
    return -1;
}

static const uint8_t* split(const uint8_t (*pairs)[3], size_t count, int together) {
    for (size_t i = 0; i < count; i++)
        if (pairs[i][2] == together) return pairs[i];
    return NULL;
}

uint32_t hangul_preedit(const hangul* h) {
    if (h->initial >= 0 && h->medial >= 0) return 0xac00 + ((uint32_t)h->initial * 21 + (uint32_t)h->medial) * 28 + h->final;
    if (h->initial >= 0) return lone[h->initial];
    return h->medial >= 0 ? 0x314f + (uint32_t)h->medial : 0;
}

size_t hangul_flush(hangul* h, char* out) {
    uint32_t c = hangul_preedit(h);
    *h = HANGUL_EMPTY;
    if (c == 0) return 0;
    out[0] = (char)(0xe0 | c >> 12), out[1] = (char)(0x80 | (c >> 6 & 0x3f)), out[2] = (char)(0x80 | (c & 0x3f));
    return 3;
}

bool hangul_key(hangul* h, char letter, char* out, size_t* written) {
    int initial = find(initials, letter), medial = find(medials, letter), together;
    *written = 0;
    if (initial < 0 && medial < 0) return false;
    if (initial >= 0) {
        if (h->initial >= 0 && h->medial >= 0 && h->final == 0 && finals[initial] != 0) {
            h->final = finals[initial];
        } else if (h->initial >= 0 && h->medial >= 0 && h->final != 0 &&
                   (together = pair(double_finals, sizeof double_finals / 3, h->final, initial)) >= 0) {
            h->final = (uint8_t)together;
        } else {
            *written = hangul_flush(h, out);
            h->initial = (int8_t)initial;
        }
        return true;
    }
    if (h->medial >= 0 && h->final == 0 && (together = pair(double_medials, sizeof double_medials / 3, h->medial, medial)) >= 0) {
        h->medial = (int8_t)together;
    } else if (h->final != 0) {
        // The final starts the next syllable; of a double final, its second consonant
        const uint8_t* d = split(double_finals, sizeof double_finals / 3, h->final);
        int next = d != NULL ? d[1] : 0;
        while (d == NULL && finals[next] != h->final)
            next++;
        h->final = d != NULL ? d[0] : 0;
        *written = hangul_flush(h, out);
        *h = (hangul){(int8_t)next, (int8_t)medial, 0};
    } else if (h->initial >= 0 && h->medial < 0) {
        h->medial = (int8_t)medial;
    } else {
        *written = hangul_flush(h, out);
        h->medial = (int8_t)medial;
    }
    return true;
}

bool hangul_backspace(hangul* h) {
    const uint8_t* d;
    if (h->final != 0)
        h->final = (d = split(double_finals, sizeof double_finals / 3, h->final)) != NULL ? d[0] : 0;
    else if (h->medial >= 0)
        h->medial = (int8_t)((d = split(double_medials, sizeof double_medials / 3, h->medial)) != NULL ? d[0] : -1);
    else if (h->initial >= 0)
        h->initial = -1;
    else
        return false;
    return true;
}
