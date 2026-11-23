#include "notify.h"
#include <string.h>

bool notify_text(const char *state, char *out, size_t n)
{
    if (!state || !state[0] || !strcmp(state, "unknown") || !strcmp(state, "unavailable") || !n) return false;
    /* HA's text box has no Enter: a typed \n is a line break, \\ a backslash */
    size_t len = 0;
    for (const char *p = state; *p && len < n - 1; p++) {
        if (p[0] == '\\' && p[1] == 'n') { out[len++] = '\n'; p++; }
        else if (p[0] == '\\' && p[1] == '\\') { out[len++] = '\\'; p++; }
        else out[len++] = *p;
    }
    /* cut short: not inside a UTF-8 character */
    if (len == n - 1 && ((unsigned char)out[len - 1] & 0x80)) {
        size_t k = len;
        while (k > 0 && ((unsigned char)out[k - 1] & 0xC0) == 0x80) k--;   /* start of the last char */
        if (k > 0) {
            unsigned char c = out[k - 1];
            size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            if (len - (k - 1) < need) len = k - 1;
        }
    }
    out[len] = 0;
    return true;
}

bool notify_new(notify_seen_t *seen, const char *state, char *out, size_t n)
{
    char t[NOTIFY_MAX];
    if (!notify_text(state, t, sizeof(t))) {
        if (state && !state[0]) seen->last[0] = 0;   /* emptied: the same text may come again */
        return false;
    }
    if (!strcmp(t, seen->last)) return false;
    memcpy(seen->last, t, sizeof(t));
    return notify_text(state, out, n);
}
