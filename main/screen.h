/* Backlight level for night mode and the screen-off timeout (issue #17). Pure, host-tested. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SCREEN_WAKE_PCT 10      /* a touch on a screen that should be off shows this much */
#define SCREEN_WAKE_MS  30000   /* ...for this long when there is no idle timeout */

typedef struct {
    int normal_pct;      /* 5..100, day brightness */
    int night_pct;       /* 0..100, 0 = off */
    bool night;
    int timeout_min;     /* 0 = never */
    bool presenting;     /* presentation mode cycling: no idle timeout */
    uint32_t idle_ms;    /* since the last touch */
    bool woken;          /* the last touch woke a screen whose level was 0 */
} screen_in_t;

/* Backlight % to set, 0 = off */
int screen_level(const screen_in_t *in);
