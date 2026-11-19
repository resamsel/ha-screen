#include "names.h"
#include <stdbool.h>
#include <string.h>

#define MAX_WORDS 12

/* Splits s at spaces; returns the word count, starts/lengths in st/ln */
static int words(const char *s, int st[], int ln[])
{
    int n = 0, i = 0;
    while (s[i] && n < MAX_WORDS) {
        while (s[i] == ' ') i++;
        if (!s[i]) break;
        st[n] = i;
        while (s[i] && s[i] != ' ') i++;
        ln[n] = i - st[n];
        n++;
    }
    return n;
}

static bool same(const char *a, int as, int al, const char *b, int bs, int bl)
{
    return al == bl && !memcmp(a + as, b + bs, al);
}

void names_abbreviate(char names[][32], int n, int max_w, int (*width)(const char *s, void *ctx), void *ctx)
{
    if (n < 2) return;
    bool too_wide = false;
    for (int i = 0; i < n; i++) too_wide |= width(names[i], ctx) > max_w;
    if (!too_wide) return;

    int st[8][MAX_WORDS], ln[8][MAX_WORDS], wc[8], minw = MAX_WORDS;
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        wc[i] = words(names[i], st[i], ln[i]);
        if (wc[i] < minw) minw = wc[i];
    }
    /* shared leading words, then shared trailing words; at least one word must remain */
    int pre = 0, suf = 0;
    while (pre < minw - 1) {
        bool ok = true;
        for (int i = 1; i < n && ok; i++)
            ok = same(names[0], st[0][pre], ln[0][pre], names[i], st[i][pre], ln[i][pre]);
        if (!ok) break;
        pre++;
    }
    while (pre + suf < minw - 1) {
        int k0 = wc[0] - 1 - suf;
        bool ok = true;
        for (int i = 1; i < n && ok; i++) {
            int k = wc[i] - 1 - suf;
            ok = same(names[0], st[0][k0], ln[0][k0], names[i], st[i][k], ln[i][k]);
        }
        if (!ok) break;
        suf++;
    }
    if (!pre && !suf) return;

    char out[8][32];
    for (int i = 0; i < n; i++) {
        int a = st[i][pre], last = wc[i] - 1 - suf;
        int len = st[i][last] + ln[i][last] - a;
        memcpy(out[i], names[i] + a, len);
        out[i][len] = 0;
    }
    bool all_equal = true;
    for (int i = 1; i < n; i++) all_equal &= !strcmp(out[0], out[i]);
    if (all_equal) return;
    for (int i = 0; i < n; i++) strcpy(names[i], out[i]);
}
