#include "live.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

/* Only what the display reads; numbers are kept as text. */
typedef struct {
    char id[64];
    bool known;          /* a state was received */
    char state[48];
    char name[32];       /* friendly_name */
    char unit[12];       /* unit_of_measurement */
    char icon[48];
    char temp[12], hum[12], tunit[8];   /* weather */
    char min[12], max[12];              /* input_number */
} ent_t;

struct live {
    int n;
    ent_t e[];
};

live_t *live_new(const char *const *ids, int n)
{
    live_t *l = calloc(1, sizeof(*l) + n * sizeof(ent_t));
    if (!l) return NULL;
    l->n = n;
    for (int i = 0; i < n; i++) snprintf(l->e[i].id, sizeof(l->e[i].id), "%s", ids[i]);
    return l;
}

void live_free(live_t *l) { free(l); }
int live_count(const live_t *l) { return l ? l->n : 0; }
const char *live_id(const live_t *l, int i) { return l && i >= 0 && i < l->n ? l->e[i].id : NULL; }

int live_find(const live_t *l, const char *id)
{
    for (int i = 0; l && id && i < l->n; i++)
        if (!strcmp(l->e[i].id, id)) return i;
    return -1;
}

/* A string or number attribute as text; quotes become apostrophes (the output is JSON) */
static bool val_text(const cJSON *v, char *out, size_t n)
{
    if (cJSON_IsString(v) && v->valuestring) {
        snprintf(out, n, "%s", v->valuestring);
        for (char *p = out; *p; p++) if (*p == '"' || *p == '\\') *p = '\'';
        return true;
    }
    if (cJSON_IsNumber(v)) {
        double d = v->valuedouble;
        if (d == (long long)d) snprintf(out, n, "%lld", (long long)d);
        else snprintf(out, n, "%g", d);
        return true;
    }
    return false;
}

static bool set(char *field, size_t n, const cJSON *v)
{
    char tmp[64];
    if (n > sizeof(tmp)) n = sizeof(tmp);
    if (!val_text(v, tmp, n) || !strcmp(field, tmp)) return false;   /* cut to the field first */
    memcpy(field, tmp, n);
    return true;
}

#define ATTR(f, key) { #key, offsetof(ent_t, f), sizeof(((ent_t *)0)->f) }
static const struct { const char *key; size_t off, size; } ATTRS[] = {
    ATTR(name, friendly_name), ATTR(unit, unit_of_measurement), ATTR(icon, icon),
    ATTR(temp, temperature), ATTR(hum, humidity), ATTR(tunit, temperature_unit),
    ATTR(min, min), ATTR(max, max),
};
#define NATTRS (int)(sizeof(ATTRS) / sizeof(ATTRS[0]))

/* s = state, a = attributes object; full = forget attributes not in a */
static bool update(ent_t *e, const cJSON *s, const cJSON *a, bool full)
{
    bool ch = false;
    if (s) ch |= set(e->state, sizeof(e->state), s);
    for (int k = 0; k < NATTRS; k++) {
        char *f = (char *)e + ATTRS[k].off;
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, ATTRS[k].key);
        if (v) ch |= set(f, ATTRS[k].size, v);
        else if (full && f[0]) { f[0] = 0; ch = true; }
    }
    if (!e->known && s) { e->known = true; ch = true; }
    return ch;
}

static void mark(int *changed, int *nc, int max, int i)
{
    for (int k = 0; k < *nc; k++) if (changed[k] == i) return;
    if (*nc < max) changed[(*nc)++] = i;
}

int live_apply(live_t *l, const char *msg, int *changed, int max)
{
    int nc = 0;
    cJSON *root = l && msg ? cJSON_Parse(msg) : NULL;
    const cJSON *ev = cJSON_GetObjectItemCaseSensitive(root, "event"), *x;
    if (!cJSON_IsObject(ev)) { cJSON_Delete(root); return 0; }

    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(ev, "a")) {
        int i = live_find(l, x->string);
        if (i < 0) continue;
        if (update(&l->e[i], cJSON_GetObjectItemCaseSensitive(x, "s"), cJSON_GetObjectItemCaseSensitive(x, "a"), true))
            mark(changed, &nc, max, i);
    }
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(ev, "c")) {
        int i = live_find(l, x->string);
        if (i < 0) continue;
        ent_t *e = &l->e[i];
        const cJSON *plus = cJSON_GetObjectItemCaseSensitive(x, "+");
        bool ch = plus && update(e, cJSON_GetObjectItemCaseSensitive(plus, "s"), cJSON_GetObjectItemCaseSensitive(plus, "a"), false);
        const cJSON *gone, *minus = cJSON_GetObjectItemCaseSensitive(x, "-");
        cJSON_ArrayForEach(gone, cJSON_GetObjectItemCaseSensitive(minus, "a")) {
            for (int k = 0; cJSON_IsString(gone) && k < NATTRS; k++) {
                char *f = (char *)e + ATTRS[k].off;
                if (!strcmp(gone->valuestring, ATTRS[k].key) && f[0]) { f[0] = 0; ch = true; }
            }
        }
        if (ch) mark(changed, &nc, max, i);
    }
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(ev, "r")) {
        int i = cJSON_IsString(x) ? live_find(l, x->valuestring) : -1;
        if (i < 0) continue;
        ent_t *e = &l->e[i];
        char id[64];
        memcpy(id, e->id, sizeof(id));
        memset(e, 0, sizeof(*e));
        memcpy(e->id, id, sizeof(id));
        e->known = true;
        snprintf(e->state, sizeof(e->state), "unavailable");   /* removed: shown like HA does */
        mark(changed, &nc, max, i);
    }
    cJSON_Delete(root);
    return nc;
}

bool live_json(const live_t *l, int i, char *out, size_t n)
{
    if (!l || i < 0 || i >= l->n || !l->e[i].known) return false;
    const ent_t *e = &l->e[i];
    int w = snprintf(out, n, "{\"entity_id\":\"%s\",\"state\":\"%s\",\"attributes\":{", e->id, e->state);
    bool first = true;
    for (int k = 0; k < NATTRS && w > 0 && (size_t)w < n; k++) {
        const char *f = (const char *)e + ATTRS[k].off;
        if (!f[0]) continue;
        bool num = !strcmp(ATTRS[k].key, "temperature") || !strcmp(ATTRS[k].key, "humidity") ||
                   !strcmp(ATTRS[k].key, "min") || !strcmp(ATTRS[k].key, "max");
        w += snprintf(out + w, n - w, num ? "%s\"%s\":%s" : "%s\"%s\":\"%s\"", first ? "" : ",", ATTRS[k].key, f);
        first = false;
    }
    if (w > 0 && (size_t)w < n) w += snprintf(out + w, n - w, "}}");
    return w > 0 && (size_t)w < n;
}
