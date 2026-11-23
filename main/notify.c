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
