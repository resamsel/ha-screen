#include "theme.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "cJSON.h"

static const char *const KEY[THEME_N] = {
    "primary-color", "accent-color", "primary-background-color", "card-background-color",
    "primary-text-color", "secondary-text-color",
};

static int hexv(char c) { return isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10); }

bool theme_color(const char *css, uint32_t *rgb)
{
    while (*css == ' ') css++;
    if (*css == '#') {
        size_t n = strspn(css + 1, "0123456789abcdefABCDEF");
        if (n == 3 || n == 4) {           /* #rgb(a) */
            *rgb = (uint32_t)(hexv(css[1]) * 17) << 16 | (uint32_t)(hexv(css[2]) * 17) << 8 | (uint32_t)(hexv(css[3]) * 17);
            return true;
        }
        if (n == 6 || n == 8) {           /* #rrggbb(aa) */
            uint32_t v = 0;
            for (int i = 1; i <= 6; i++) v = v << 4 | (uint32_t)hexv(css[i]);
            *rgb = v;
            return true;
        }
        return false;
    }
    int r, g, b;
    if (sscanf(css, "rgb(%d ,%d ,%d", &r, &g, &b) == 3 || sscanf(css, "rgba(%d ,%d ,%d", &r, &g, &b) == 3 ||
        sscanf(css, "rgb(%d %d %d", &r, &g, &b) == 3) {
        if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) return false;
        *rgb = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        return true;
    }
    return false;
}

/* key's value in the mode's overrides, else the theme's own; "var(--x)" is looked up once */
static const char *lookup(const cJSON *base, const cJSON *mode, const char *key, int depth)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(mode, key);
    if (!cJSON_IsString(v)) v = cJSON_GetObjectItemCaseSensitive(base, key);
    if (!cJSON_IsString(v)) return NULL;
    const char *s = v->valuestring;
    if (!strncmp(s, "var(--", 6) && depth < 3) {
        char ref[64];
        size_t n = strcspn(s + 6, ",)");
        if (n >= sizeof(ref)) return NULL;
        memcpy(ref, s + 6, n);
        ref[n] = 0;
        const char *r = lookup(base, mode, ref, depth + 1);
        if (r) return r;
        const char *fb = s[6 + n] == ',' ? s + 7 + n : NULL;   /* var(--x, fallback) */
        return fb;
    }
    return s;
}

bool theme_parse(const char *json, const char *name, ha_theme_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!cJSON_IsObject(res)) res = root;
    const cJSON *themes = cJSON_GetObjectItemCaseSensitive(res, "themes");
    bool any = false;
    for (int m = 0; m < 2; m++) {         /* 0 dark, 1 light */
        const char *nm = name && name[0] ? name : NULL;
        if (!nm && m == 0) {
            const cJSON *d = cJSON_GetObjectItemCaseSensitive(res, "default_dark_theme");
            if (cJSON_IsString(d) && d->valuestring[0]) nm = d->valuestring;
        }
        if (!nm) {
            const cJSON *d = cJSON_GetObjectItemCaseSensitive(res, "default_theme");
            if (cJSON_IsString(d)) nm = d->valuestring;
        }
        const cJSON *th = nm ? cJSON_GetObjectItemCaseSensitive(themes, nm) : NULL;
        if (!cJSON_IsObject(th)) continue;
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(th, "modes"), m ? "light" : "dark");
        for (int c = 0; c < THEME_N; c++) {
            const char *v = lookup(th, mode, KEY[c], 0);
            if (v && theme_color(v, &out->rgb[m][c])) { out->has[m] |= (uint8_t)(1u << c); any = true; }
        }
    }
    cJSON_Delete(root);
    return any;
}
