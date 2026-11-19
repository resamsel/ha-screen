/* Host tests for main/names.c. Build/run: see test/run.sh */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "names.h"
#include "tz.h"

static int w10(const char *s, void *ctx) { (void)ctx; return (int)strlen(s) * 10; }

static void set(char n[][32], const char **in, int k) { for (int i = 0; i < k; i++) strcpy(n[i], in[i]); }

static void test_fits_unchanged(void)
{
    char n[3][32];
    const char *in[] = { "Temperatur OG", "Temperatur EG", "Temperatur UG" };
    set(n, in, 3);
    names_abbreviate(n, 3, 200, w10, NULL);
    assert(!strcmp(n[0], "Temperatur OG") && !strcmp(n[2], "Temperatur UG"));
}

static void test_common_prefix(void)
{
    char n[3][32];
    const char *in[] = { "Temperatur OG", "Temperatur EG", "Temperatur UG" };
    set(n, in, 3);
    names_abbreviate(n, 3, 100, w10, NULL);
    assert(!strcmp(n[0], "OG") && !strcmp(n[1], "EG") && !strcmp(n[2], "UG"));
}

static void test_common_suffix_and_prefix(void)
{
    char n[2][32];
    const char *in[] = { "Sensor Kueche Temperatur", "Sensor Bad Temperatur" };
    set(n, in, 2);
    names_abbreviate(n, 2, 100, w10, NULL);
    assert(!strcmp(n[0], "Kueche") && !strcmp(n[1], "Bad"));
}

static void test_whole_words_only(void)
{
    char n[2][32];
    const char *in[] = { "Temperature A", "Temperatur B" };   /* "Temperatur" isn't a shared word */
    set(n, in, 2);
    names_abbreviate(n, 2, 50, w10, NULL);
    assert(!strcmp(n[0], "Temperature A") && !strcmp(n[1], "Temperatur B"));
}

static void test_never_empty(void)
{
    char n[2][32];
    const char *in[] = { "Wohnzimmer Licht", "Wohnzimmer Licht" };   /* identical: keep */
    set(n, in, 2);
    names_abbreviate(n, 2, 50, w10, NULL);
    assert(!strcmp(n[0], "Wohnzimmer Licht"));
    const char *in2[] = { "Bad Temp", "Bad" };   /* stripping "Bad" would empty the second: keep all */
    set(n, in2, 2);
    names_abbreviate(n, 2, 50, w10, NULL);
    assert(!strcmp(n[0], "Bad Temp") && !strcmp(n[1], "Bad"));
}

static void test_single_and_dedupe_spaces(void)
{
    char n[1][32];
    strcpy(n[0], "FRITZ!Box 7590 AX Download");
    names_abbreviate(n, 1, 50, w10, NULL);   /* nothing shared: LVGL's ellipsis handles it */
    assert(!strcmp(n[0], "FRITZ!Box 7590 AX Download"));
}

static void test_state_text(void)
{
    char o[24];
    state_text("not_charging", o, sizeof(o));
    assert(!strcmp(o, "Not charging"));
    state_text("middle", o, sizeof(o));
    assert(!strcmp(o, "Middle"));
    state_text("on", o, sizeof(o));
    assert(!strcmp(o, "On"));
    state_text("", o, sizeof(o));
    assert(!strcmp(o, "--"));
    state_text("a_very_long_state_value_that_does_not_fit", o, sizeof(o));
    assert(strlen(o) == sizeof(o) - 1);
    state_text("\xc3\xa4_x", o, sizeof(o));   /* non-ASCII first letter stays as is */
    assert(!strcmp(o, "\xc3\xa4 x"));
    char o4[4];
    state_text("ab\xc3\xa4", o4, sizeof(o4));   /* "ä" would be cut in half: dropped */
    assert(!strcmp(o4, "Ab"));
}

static void test_tz(void)
{
    assert(!strcmp(tz_posix("Europe/Vienna"), "CET-1CEST,M3.5.0,M10.5.0/3"));
    assert(!strcmp(tz_posix("Europe/London"), "GMT0BST,M3.5.0/1,M10.5.0"));
    assert(!strcmp(tz_posix("America/New_York"), "EST5EDT,M3.2.0,M11.1.0"));
    assert(!strcmp(tz_posix("UTC"), "UTC0"));
    assert(tz_posix("Mars/Olympus") == NULL);
    assert(tz_posix("") == NULL);
}

int main(void)
{
    test_tz();
    test_state_text();
    test_fits_unchanged();
    test_common_prefix();
    test_common_suffix_and_prefix();
    test_whole_words_only();
    test_never_empty();
    test_single_and_dedupe_spaces();
    puts("all names tests passed");
    return 0;
}
