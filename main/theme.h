/* Home Assistant theme colours (issue #51). Pure, host-tested. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { THEME_PRIMARY, THEME_ACCENT, THEME_BG, THEME_CARD, THEME_TEXT, THEME_TEXT2, THEME_N };

typedef struct {
    uint32_t rgb[2][THEME_N];   /* [dark, light][colour] as 0xRRGGBB */
    uint8_t has[2];             /* bit c: rgb[mode][c] is set (else the board's own colour) */
} ha_theme_t;

/* "#rgb", "#rrggbb", "rgb(r, g, b)" or "rgba(r, g, b, a)" -> 0xRRGGBB */
bool theme_color(const char *css, uint32_t *rgb);

/* frontend/get_themes result message (or its "result" object). name "" = HA's default theme
 * (default_dark_theme for dark mode when set). Zeroes *out first; false if the theme
 * wasn't found (the default theme "default" has no colours: false too). */
bool theme_parse(const char *json, const char *name, ha_theme_t *out);
