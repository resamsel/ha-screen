#include "icon_rule.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Rule string: the default icon, then one line per state ("s<state>=<icon>") or range
 * threshold ("r<number>=<icon>"), e.g. "mdi:toggle-switch-variant\nsoff=mdi:toggle-switch-variant-off" */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    return p;
}

static const char *skip_str(const char *p)   /* p at '"'; returns after the closing quote */
{
    for (p++; *p && *p != '"'; p++)
        if (*p == '\\' && p[1]) p++;
    return *p ? p + 1 : p;
}

static const char *skip_value(const char *p)
{
    p = skip_ws(p);
    if (*p == '"') return skip_str(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = skip_str(p); continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;   /* number, true, false, null */
    return p;
}

bool json_get_str(const char *p, char *out, size_t n)
{
    if (!p || *p != '"' || !n) return false;
    size_t k = 0;
    for (p++; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1]) p++;
        if (k + 1 < n) out[k++] = *p;
    }
    out[k] = 0;
    return *p == '"';
}

const char *json_obj_get(const char *obj, const char *key)
{
    if (!obj) return NULL;
    obj = skip_ws(obj);
    if (*obj != '{') return NULL;
    const char *p = skip_ws(obj + 1);
    size_t kl = strlen(key);
    while (*p == '"') {
        const char *k = p + 1, *ke = skip_str(p);
        p = skip_ws(ke);
        if (*p != ':') return NULL;
        p = skip_ws(p + 1);
        if ((size_t)(ke - 1 - k) == kl && !memcmp(k, key, kl)) return p;
        p = skip_ws(skip_value(p));
        if (*p != ',') return NULL;
        p = skip_ws(p + 1);
    }
    return NULL;
}

static size_t add_pairs(const char *obj, char kind, char *out, size_t len, size_t n)
{
    if (!obj || *obj != '{') return len;
    const char *p = skip_ws(obj + 1);
    while (*p == '"') {
        char key[32], icon[48];
        json_get_str(p, key, sizeof(key));
        p = skip_ws(skip_str(p));
        if (*p != ':') break;
        p = skip_ws(p + 1);
        if (json_get_str(p, icon, sizeof(icon))) {
            int w = snprintf(out + len, n - len, "\n%c%s=%s", kind, key, icon);
            if (w > 0 && len + w < n) len += w;
        }
        p = skip_ws(skip_value(p));
        if (*p != ',') break;
        p = skip_ws(p + 1);
    }
    return len;
}

bool icon_rule_from_icons(const char *msg, const char *integration, const char *domain, const char *key,
                          char *out, size_t n)
{
    out[0] = 0;
    const char *res = json_obj_get(json_obj_get(msg, "result"), "resources");
    const char *e = json_obj_get(res, integration);
    if (domain) e = json_obj_get(e, domain);   /* entity icons: per domain; component icons: not */
    e = json_obj_get(e, key);
    if (!e || *e != '{' || n < 2) return false;
    char def[48] = "";
    json_get_str(json_obj_get(e, "default"), def, sizeof(def));
    size_t len = snprintf(out, n, "%s", def);
    if (len >= n) len = n - 1;
    len = add_pairs(json_obj_get(e, "state"), 's', out, len, n);
    add_pairs(json_obj_get(e, "range"), 'r', out, len, n);
    return out[0] != 0;
}

void icon_rule_apply(const char *rule, const char *state, char *out, size_t n)
{
    out[0] = 0;
    if (!rule || !n) return;
    const char *nl = strchr(rule, '\n');
    size_t dl = nl ? (size_t)(nl - rule) : strlen(rule);
    snprintf(out, n, "%.*s", (int)dl, rule);   /* default */
    char *end;
    double v = state ? strtod(state, &end) : 0;
    bool num = state && end != state && !*end;
    double best = -1e300;
    for (const char *l = nl; l; l = strchr(l + 1, '\n')) {
        const char *k = l + 2, *eq = strchr(k, '='), *le = strchr(k, '\n');
        if (!eq || (le && eq > le)) continue;
        size_t il = le ? (size_t)(le - eq - 1) : strlen(eq + 1);
        if (l[1] == 's' && state && strlen(state) == (size_t)(eq - k) && !memcmp(k, state, eq - k)) {
            snprintf(out, n, "%.*s", (int)il, eq + 1);
            return;
        }
        if (l[1] == 'r' && num) {
            double t = strtod(k, NULL);
            if (t <= v && t > best) { best = t; snprintf(out, n, "%.*s", (int)il, eq + 1); }
        }
    }
}
