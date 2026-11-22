/* Host tests for main/screen.c. Build/run: see test/run.sh */
#include <assert.h>
#include <stdio.h>
#include "screen.h"

static screen_in_t base(void) { return (screen_in_t){ .normal_pct = 60 }; }

int main(void)
{
    screen_in_t s = base();
    assert(screen_level(&s) == 60);                                  /* day_active */
    s.timeout_min = 5; s.idle_ms = 5 * 60000;     assert(screen_level(&s) == 0);   /* day_timeout */
    s.idle_ms = 5 * 60000 - 1;                    assert(screen_level(&s) == 60);
    s = base(); s.idle_ms = 36000000;             assert(screen_level(&s) == 60);  /* never_timeout */
    s = base(); s.timeout_min = 1; s.idle_ms = 600000; s.presenting = true;
    assert(screen_level(&s) == 60);                                  /* presenting_keeps_on */
    s = base(); s.night = true; s.night_pct = 15; assert(screen_level(&s) == 15);  /* night_dim */
    s = base(); s.night = true; s.idle_ms = 600000; assert(screen_level(&s) == 0); /* night_zero_is_off */
    s.presenting = true;                          assert(screen_level(&s) == 0);   /* night_presenting */
    s = base(); s.night = true; s.woken = true;   assert(screen_level(&s) == 10);  /* wake_fallback */
    s.idle_ms = 30000;                            assert(screen_level(&s) == 0);   /* expires, never */
    s.timeout_min = 2; s.idle_ms = 60000;         assert(screen_level(&s) == 10);
    s.idle_ms = 120000;                           assert(screen_level(&s) == 0);
    puts("screen_test: ok");
    return 0;
}
