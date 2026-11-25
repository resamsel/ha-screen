/* Host tests for main/theme.c. Build/run: see test/run.sh */
#include <assert.h>
#include <stdio.h>
#include "theme.h"

int main(void)
{
    uint32_t c;
    assert(theme_color("#03a9f4", &c) && c == 0x03A9F4);
    assert(theme_color(" #FFF", &c) && c == 0xFFFFFF);
    assert(theme_color("#11223380", &c) && c == 0x112233);
    assert(theme_color("rgb(1, 2, 3)", &c) && c == 0x010203);
    assert(theme_color("rgba(255,0,16,0.5)", &c) && c == 0xFF0010);
    assert(!theme_color("var(--x)", &c) && !theme_color("red", &c) && !theme_color("#12", &c));

    const char *j = "{\"id\":1,\"type\":\"result\",\"success\":true,\"result\":{\"themes\":{"
        "\"Ocean\":{\"primary-color\":\"#0077be\",\"accent-color\":\"var(--primary-color)\","
        "  \"card-background-color\":\"var(--nope, #101010)\","
        "  \"modes\":{\"dark\":{\"primary-background-color\":\"#000a14\",\"primary-text-color\":\"rgb(220, 230, 240)\"},"
        "           \"light\":{\"primary-background-color\":\"#f0f8ff\"}}},"
        "\"Night\":{\"primary-color\":\"#ff9800\"}},"
        "\"default_theme\":\"Ocean\",\"default_dark_theme\":\"Night\"}}";
    ha_theme_t t;
    assert(theme_parse(j, "Ocean", &t));
    assert(t.rgb[1][THEME_PRIMARY] == 0x0077BE && t.rgb[1][THEME_ACCENT] == 0x0077BE);   /* var() */
    assert(t.rgb[1][THEME_CARD] == 0x101010);                                            /* var() fallback */
    assert(t.rgb[0][THEME_BG] == 0x000A14 && t.rgb[1][THEME_BG] == 0xF0F8FF);            /* modes */
    assert((t.has[0] & (1 << THEME_TEXT)) && !(t.has[1] & (1 << THEME_TEXT)));
    assert(!(t.has[1] & (1 << THEME_TEXT2)));
    assert(theme_parse(j, "", &t));                                                      /* defaults */
    assert(t.rgb[0][THEME_PRIMARY] == 0xFF9800 && t.has[0] == 1 << THEME_PRIMARY);       /* dark: default_dark_theme */
    assert(t.rgb[1][THEME_PRIMARY] == 0x0077BE);
    assert(!theme_parse(j, "Missing", &t) && !t.has[0] && !t.has[1]);
    assert(!theme_parse("{\"result\":{\"themes\":{},\"default_theme\":\"default\"}}", "", &t));
    assert(!theme_parse(NULL, "", &t));
    puts("theme_test: ok");
    return 0;
}
