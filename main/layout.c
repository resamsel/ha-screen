#define _POSIX_C_SOURCE 200809L   /* strtok_r on the host build */
#include <stdio.h>
#include "layout.h"
#include <string.h>
#include "cJSON.h"

#ifdef LAYOUT_HOST
#include <stdio.h>
#define LOGW(fmt, ...) fprintf(stderr, "layout: " fmt "\n", ##__VA_ARGS__)
#else
#include "esp_log.h"
#define LOGW(fmt, ...) ESP_LOGW("layout", fmt, ##__VA_ARGS__)
#endif

static void copy(char *dst, size_t n, const char *src)
{
    strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
}

static const char *str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

static bool is_graph(const cJSON *card)
{
    const char *type = str(card, "type"), *graph = str(card, "graph");
    return type && graph && !strcmp(type, "sensor") && !strcmp(graph, "line");
}

static bool is_button(const cJSON *card)
{
    const char *type = str(card, "type");
    return type && !strcmp(type, "button");
}

static bool is_tile(const cJSON *card)
{
    const char *type = str(card, "type");
    return type && !strcmp(type, "tile");
}

static bool known_part(const char *t)
{
    return !strcmp(t, "floor") || !strcmp(t, "area") || !strcmp(t, "device") || !strcmp(t, "entity");
}

/* One name part {"type":"floor"} / {"type":"text","text":"x"} appended to name_type */
static void add_part(layout_item_t *it, const cJSON *part)
{
    const char *t = str(part, "type");
    char buf[48];
    if (!t) return;
    if (!strcmp(t, "text")) {
        const char *x = str(part, "text");
        if (!x) return;
        snprintf(buf, sizeof(buf), "text:%s", x);
    } else if (known_part(t)) {
        snprintf(buf, sizeof(buf), "%s", t);
    } else {
        LOGW("name type '%s' not supported, skipped", t);
        return;
    }
    size_t n = strlen(it->name_type);
    if (n + strlen(buf) + 2 > sizeof(it->name_type)) return;
    if (n) strcat(it->name_type, "|");
    strcat(it->name_type, buf);
}

static void set_item(layout_item_t *it, const cJSON *card)
{
    copy(it->entity, sizeof(it->entity), str(card, "entity"));
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(card, "name");
    if (cJSON_IsString(name)) {
        copy(it->name, sizeof(it->name), name->valuestring);
    } else if (cJSON_IsObject(name)) {
        const char *t = str(name, "type"), *x = str(name, "text");
        if (t && !strcmp(t, "text") && x) copy(it->name, sizeof(it->name), x);   /* plain text */
        else add_part(it, name);
    } else if (cJSON_IsArray(name)) {
        const cJSON *p;
        cJSON_ArrayForEach(p, name) if (cJSON_IsObject(p)) add_part(it, p);
    }
}

/* Fills *pg from one section; returns false if nothing usable. */
static bool parse_section(const cJSON *sec, int idx, layout_page_t *pg)
{
    const char *type = str(sec, "type");
    if (type && strcmp(type, "grid")) {
        LOGW("section %d: unknown type '%s', skipped", idx, type);
        return false;
    }
    const cJSON *cards = cJSON_GetObjectItemCaseSensitive(sec, "cards");
    if (!cJSON_IsArray(cards)) return false;

    memset(pg, 0, sizeof(*pg));
    bool kind_set = false;
    const cJSON *card;
    cJSON_ArrayForEach(card, cards) {
        if (!cJSON_IsObject(card)) continue;
        const char *ct = str(card, "type");
        if (ct && !strcmp(ct, "heading")) {
            const char *h = str(card, "heading");
            if (h && !pg->title[0]) copy(pg->title, sizeof(pg->title), h);
            continue;
        }
        if (ct && !strcmp(ct, "clock")) {   /* no entity: the board's own time */
            if (kind_set) {
                LOGW("section %d: clock only as the first card of a section, skipped", idx);
                continue;
            }
            pg->kind = LAYOUT_CLOCK;
            kind_set = true;
            layout_item_t *it = &pg->items[pg->count++];
            const cJSON *sec_ = cJSON_GetObjectItemCaseSensitive(card, "show_seconds");
            const char *tf = str(card, "time_format");
            if (cJSON_IsTrue(sec_)) it->flags |= LAYOUT_CLOCK_SECONDS;
            if (tf && !strcmp(tf, "12")) it->flags |= LAYOUT_CLOCK_12H;
            const char *sz = str(card, "clock_size");
            if (sz && !strcmp(sz, "medium")) it->flags |= LAYOUT_CLOCK_MEDIUM;
            else if (sz && !strcmp(sz, "large")) it->flags |= LAYOUT_CLOCK_LARGE;
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(card, "no_background"))) it->flags |= LAYOUT_CLOCK_NO_BG;
            const char *tz = str(card, "time_zone");
            if (tz) copy(it->entity, sizeof(it->entity), tz);
            continue;
        }
        if (pg->kind == LAYOUT_CLOCK && kind_set) {
            LOGW("section %d: clock page shows only the clock, card '%s' skipped", idx, ct ? ct : "?");
            continue;
        }
        bool graph = is_graph(card), tile = is_tile(card), button = is_button(card);
        if (!graph && !tile && !button) {
            LOGW("section %d: card '%s' not supported, skipped", idx, ct ? ct : "?");
            continue;
        }
        if (!str(card, "entity")) {
            LOGW("section %d: card without entity, skipped", idx);
            continue;
        }
        if (!kind_set) {
            pg->kind = graph ? LAYOUT_GRAPH : button ? LAYOUT_BUTTONS : LAYOUT_VALUES;
            kind_set = true;
        }
        if (pg->kind == LAYOUT_GRAPH) {
            if (!graph) {
                LOGW("section %d: only graphs on a graph page, card skipped", idx);
                continue;
            }
            if (pg->count == LAYOUT_MAX_ROWS) {
                LOGW("section %d: more than %d graphs, extra skipped", idx, LAYOUT_MAX_ROWS);
                continue;
            }
        } else if (pg->kind == LAYOUT_BUTTONS) {
            if (!button) {
                LOGW("section %d: only buttons on a button page, card skipped", idx);
                continue;
            }
            if (pg->count == LAYOUT_MAX_ROWS) {
                LOGW("section %d: more than %d buttons, extra skipped", idx, LAYOUT_MAX_ROWS);
                continue;
            }
        } else {
            if (!tile) {
                LOGW("section %d: only tiles on a value page, card skipped", idx);
                continue;
            }
            if (pg->count == LAYOUT_MAX_ROWS) {
                LOGW("section %d: more than %d tiles, extra skipped", idx, LAYOUT_MAX_ROWS);
                continue;
            }
        }
        layout_item_t *it = &pg->items[pg->count++];
        set_item(it, card);
        if (graph) {
            const cJSON *h = cJSON_GetObjectItemCaseSensitive(card, "hours_to_show");
            int hours = cJSON_IsNumber(h) ? (int)h->valuedouble : 0;
            it->hours = hours < 1 ? LAYOUT_HOURS_DEFAULT : hours > LAYOUT_HOURS_MAX ? LAYOUT_HOURS_MAX : hours;
        }
        if (button) {
            const cJSON *ta = cJSON_GetObjectItemCaseSensitive(card, "tap_action");
            const char *act = cJSON_IsObject(ta) ? str(ta, "action") : NULL;
            it->action = !act || !strcmp(act, "toggle") ? LAYOUT_ACT_TOGGLE : LAYOUT_ACT_NONE;
            if (it->action == LAYOUT_ACT_NONE) LOGW("section %d: tap action '%s' not supported, button only shows state", idx, act);
        }
    }
    return pg->count > 0;
}

int layout_parse(const char *json, const char *view_path, ha_layout_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!json) return 0;
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        LOGW("dashboard JSON could not be parsed");
        return 0;
    }
    const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!cJSON_IsObject(cfg)) cfg = root;
    const cJSON *views = cJSON_GetObjectItemCaseSensitive(cfg, "views"), *view = NULL, *v;
    if (cJSON_IsArray(views)) {
        cJSON_ArrayForEach(v, views) {
            const char *p = str(v, "path");
            if (p && !strcmp(p, view_path)) { view = v; break; }
        }
    }
    const cJSON *secs = view ? cJSON_GetObjectItemCaseSensitive(view, "sections") : NULL;
    if (!view) LOGW("view '%s' not found", view_path);
    if (cJSON_IsArray(secs)) {
        int idx = 0;
        const cJSON *sec;
        cJSON_ArrayForEach(sec, secs) {
            if (cJSON_IsObject(sec)) {
                if (out->count == LAYOUT_MAX_PAGES) {
                    LOGW("more than %d pages, section %d skipped", LAYOUT_MAX_PAGES, idx);
                } else if (parse_section(sec, idx, &out->pages[out->count])) {
                    out->count++;
                } else {
                    memset(&out->pages[out->count], 0, sizeof(layout_page_t));
                }
            }
            idx++;
        }
    }
    cJSON_Delete(root);
    return out->count;
}

uint32_t layout_hash(const ha_layout_t *l)
{
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)l;
    for (size_t i = 0; i < sizeof(*l); i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

bool layout_name_template(const layout_item_t *it, char *out, int n)
{
    const char *e = it->entity;
    int len = snprintf(out, n, "{{ [");
    bool any = false;
    char types[sizeof(it->name_type)];
    copy(types, sizeof(types), it->name_type);
    for (char *save = NULL, *t = strtok_r(types, "|", &save); t; t = strtok_r(NULL, "|", &save)) {
        char part[400];
        if (!strncmp(t, "text:", 5)) {
            char txt[48];
            int k = 0;
            for (const char *c = t + 5; *c && k < (int)sizeof(txt) - 1; c++) if (*c != '\'' && *c != '\\') txt[k++] = *c;
            txt[k] = 0;
            snprintf(part, sizeof(part), "'%s'", txt);
        } else if (!strcmp(t, "floor")) {
            snprintf(part, sizeof(part), "floor_name('%s')", e);
        } else if (!strcmp(t, "area")) {
            snprintf(part, sizeof(part), "area_name('%s')", e);
        } else if (!strcmp(t, "device")) {
            snprintf(part, sizeof(part), "(device_attr('%s', 'name_by_user') or device_attr('%s', 'name'))", e, e);
        } else if (!strcmp(t, "entity")) {
            /* the entity's own name: friendly_name without the device name in front */
            snprintf(part, sizeof(part), "(state_attr('%s', 'friendly_name') or '') | replace("
                     "(device_attr('%s', 'name_by_user') or device_attr('%s', 'name') or '~~') ~ ' ', '', 1)", e, e, e);
        } else {
            continue;
        }
        len += snprintf(out + len, len < n ? n - len : 0, "%s%s", any ? ", " : "", part);
        any = true;
    }
    len += snprintf(out + len, len < n ? n - len : 0, "] | select | join(' ') }}");
    return any && len < n;
}

bool layout_sanitize(ha_layout_t *l)
{
    bool ok = l->count >= 1 && l->count <= LAYOUT_MAX_PAGES;
    for (int p = 0; ok && p < l->count; p++) {
        layout_page_t *pg = &l->pages[p];
        if (pg->count < 1 || pg->count > LAYOUT_MAX_ROWS) { ok = false; break; }
        if (pg->kind > LAYOUT_CLOCK) pg->kind = LAYOUT_VALUES;
        pg->title[sizeof(pg->title) - 1] = 0;
        for (int i = 0; i < pg->count; i++) {
            pg->items[i].entity[sizeof(pg->items[i].entity) - 1] = 0;
            pg->items[i].name[sizeof(pg->items[i].name) - 1] = 0;
            pg->items[i].name_type[sizeof(pg->items[i].name_type) - 1] = 0;
            if (pg->items[i].action > LAYOUT_ACT_NONE) pg->items[i].action = LAYOUT_ACT_NONE;
            pg->items[i].flags &= LAYOUT_CLOCK_FLAGS;
            if ((pg->items[i].flags & LAYOUT_CLOCK_SIZE) == LAYOUT_CLOCK_SIZE) pg->items[i].flags &= ~LAYOUT_CLOCK_SIZE;
            uint16_t *h = &pg->items[i].hours;
            if (pg->kind != LAYOUT_GRAPH) *h = 0;   /* unused; as layout_parse leaves it */
            else if (*h < 1) *h = LAYOUT_HOURS_DEFAULT;
            else if (*h > LAYOUT_HOURS_MAX) *h = LAYOUT_HOURS_MAX;
        }
    }
    if (!ok) memset(l, 0, sizeof(*l));
    return ok;
}

int layout_filter(ha_layout_t *l, bool (*keep)(const char *entity, layout_kind_t kind, void *ctx), void *ctx)
{
    int np = 0;
    for (int p = 0; p < l->count; p++) {
        layout_page_t pg = l->pages[p];
        int n = 0;
        for (int i = 0; i < pg.count; i++)
            if (pg.kind == LAYOUT_CLOCK || keep(pg.items[i].entity, pg.kind, ctx)) pg.items[n++] = pg.items[i];
        for (int i = n; i < LAYOUT_MAX_ROWS; i++) memset(&pg.items[i], 0, sizeof(layout_item_t));
        pg.count = n;
        if (n) l->pages[np++] = pg;
    }
    for (int p = np; p < LAYOUT_MAX_PAGES; p++) memset(&l->pages[p], 0, sizeof(layout_page_t));
    l->count = np;
    return np;
}
