#include "screen.h"

int screen_level(const screen_in_t *in)
{
    if (in->face_down) return 0;
    int level = in->night ? in->night_pct : in->normal_pct;
    uint32_t timeout = (uint32_t)in->timeout_min * 60000u;
    if (level <= 0) {   /* off unless a touch woke it */
        if (!in->woken) return 0;
        uint32_t wake = timeout ? timeout : SCREEN_WAKE_MS;
        return in->idle_ms < wake ? SCREEN_WAKE_PCT : 0;
    }
    if (timeout && !in->presenting && in->idle_ms >= timeout) return 0;
    return level;
}
