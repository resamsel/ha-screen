/* Backlight level for night mode and the screen-off timeout (issue #17). Pure, host-tested. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SCREEN_WAKE_PCT 10      /* a touch on a screen that should be off shows this much */
#define SCREEN_WAKE_MS  30000   /* ...for this long when there is no idle timeout */
#define SCREEN_SAVER_PCT 10     /* the clock screensaver (issue #48) at most this bright */

typedef struct {
    int normal_pct;      /* 5..100, day brightness */
    int night_pct;       /* 0..100, 0 = off */
    bool night;
    int timeout_s;       /* screen off after this idle time in seconds, 0 = never */
    bool presenting;     /* presentation mode cycling: no idle timeout */
    uint32_t idle_ms;    /* since the last touch */
    bool woken;          /* the last touch woke a screen whose level was 0 */
    bool saver;          /* show the clock instead of turning off at the timeout (issue #48) */
    bool face_down;      /* lying face down with motion wake on (issue #58): off */
} screen_in_t;

/* Backlight % to set, 0 = off */
int screen_level(const screen_in_t *in);

/* true while the clock screensaver should be shown (screen_level is then its dimmed level) */
bool screen_saver(const screen_in_t *in);
