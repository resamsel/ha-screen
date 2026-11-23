#include "notify.h"
#include <string.h>

bool notify_text(const char *state, char *out, size_t n)
{
    if (!state || !state[0] || !strcmp(state, "unknown") || !strcmp(state, "unavailable") || !n) return false;
    size_t len = strlen(state);
    if (len > n - 1) {
        len = n - 1;
        while (len > 0 && ((unsigned char)state[len] & 0xC0) == 0x80) len--;   /* not inside a character */
    }
    memcpy(out, state, len);
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
