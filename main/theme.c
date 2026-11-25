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

/* JSON string value of "key" anywhere in json (first match), unescaped simply */
static bool str_val(const char *json, const char *key, char *out, size_t n)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t k = 0;
    for (; *p && *p != '"' && k + 1 < n; p++) out[k++] = *p == '\\' && p[1] ? *++p : *p;
    out[k] = 0;
    return true;
}

/* End of the JSON value starting at p (object, string or scalar), skipping strings */
static const char *value_end(const char *p)
{
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++;
        return *p ? p + 1 : NULL;
    }
    if (*p != '{' && *p != '[') { while (*p && *p != ',' && *p != '}') p++; return p; }
    int depth = 0;
    for (; *p; p++) {
        if (*p == '"') { for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++; if (!*p) return NULL; continue; }
        if (*p == '{' || *p == '[') depth++;
        else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
    }
    return NULL;
}

/* The object of theme `name` directly under "themes", parsed on its own: get_themes lists every
 * installed theme, which can be far more than the one wanted */
static cJSON *theme_obj(const char *json, const char *name)
{
    const char *p = strstr(json, "\"themes\":{");
    if (!p) return NULL;
    p += strlen("\"themes\":{");
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p != '"') return NULL;       /* '}' : the end of "themes" */
        const char *k = p + 1, *ke = value_end(p);
        if (!ke) return NULL;
        p = ke;
        while (*p == ' ' || *p == ':') p++;
        const char *ve = value_end(p);
        if (!ve) return NULL;
        size_t kl = (size_t)(ke - 1 - k);
        if (kl == strlen(name) && !strncmp(k, name, kl)) return cJSON_ParseWithLength(p, (size_t)(ve - p));
        p = ve;
    }
    return NULL;
}

bool theme_parse(const char *json, const char *name, ha_theme_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!json) return false;
    char def[64] = "", def_dark[64] = "";
    str_val(json, "default_theme", def, sizeof(def));
    str_val(json, "default_dark_theme", def_dark, sizeof(def_dark));
    bool any = false;
    for (int m = 0; m < 2; m++) {         /* 0 dark, 1 light */
        const char *nm = name && name[0] ? name : m == 0 && def_dark[0] ? def_dark : def;
        cJSON *th = nm[0] ? theme_obj(json, nm) : NULL;
        if (!cJSON_IsObject(th)) { cJSON_Delete(th); continue; }
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(th, "modes"), m ? "light" : "dark");
        for (int c = 0; c < THEME_N; c++) {
            const char *v = lookup(th, mode, KEY[c], 0);
            if (v && theme_color(v, &out->rgb[m][c])) { out->has[m] |= (uint8_t)(1u << c); any = true; }
        }
        cJSON_Delete(th);
    }
    return any;
}
