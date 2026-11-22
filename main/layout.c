#define _POSIX_C_SOURCE 200809L   /* strtok_r on the host build */
#include <stdio.h>
#include "layout.h"
#include <string.h>
#include <math.h>
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
    return type && (!strcmp(type, "tile") || !strcmp(type, "entity"));   /* entity card: same look */
}

static float num(const cJSON *obj, const char *key, float def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

/* picture-entity card with a camera or image entity */
static bool is_picture(const cJSON *card)
{
    const char *type = str(card, "type");
    return type && (!strcmp(type, "picture-entity") || !strcmp(type, "picture-glance"));   /* glance: the image only */
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
    const char *ent = str(card, "entity");
    if (ent) copy(it->entity, sizeof(it->entity), ent);   /* picture-glance may have none */
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
/* Fills *pg from one section: every supported card becomes one item (block) with its own
 * kind, in dashboard order; returns false if nothing usable. */
static bool parse_section(const cJSON *sec, int idx, layout_page_t *pg, ha_layout_t *l)
{
    const char *type = str(sec, "type");
    if (type && strcmp(type, "grid")) {
        LOGW("section %d: unknown type '%s', skipped", idx, type);
        return false;
    }
    const cJSON *cards = cJSON_GetObjectItemCaseSensitive(sec, "cards");
    if (!cJSON_IsArray(cards)) return false;

    memset(pg, 0, sizeof(*pg));
    const cJSON *card;
    cJSON_ArrayForEach(card, cards) {
        if (!cJSON_IsObject(card)) continue;
        const char *ct = str(card, "type");
        if (ct && !strcmp(ct, "heading")) {
            const char *h = str(card, "heading");
            const char *hi = str(card, "icon");
            if (h && !pg->title[0]) {
                copy(pg->title, sizeof(pg->title), h);
                if (hi && strlen(hi) < sizeof(pg->title_icon)) copy(pg->title_icon, sizeof(pg->title_icon), hi);
            }
            continue;
        }
        if (ct && !strcmp(ct, "glance")) {   /* one cell per entity */
            const cJSON *ents = cJSON_GetObjectItemCaseSensitive(card, "entities"), *e;
            uint8_t fl = (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(card, "show_name")) ? LAYOUT_GLANCE_NO_NAME : 0) |
                         (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(card, "show_state")) ? LAYOUT_GLANCE_NO_STATE : 0);
            bool no_icon = cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(card, "show_icon"));
            cJSON_ArrayForEach(e, ents) {
                const char *ent = cJSON_IsString(e) ? e->valuestring : cJSON_IsObject(e) ? str(e, "entity") : NULL;
                if (!ent) continue;
                if (pg->count == LAYOUT_MAX_ROWS) { LOGW("section %d: more than %d cards, extra skipped", idx, LAYOUT_MAX_ROWS); break; }
                layout_item_t *it = &pg->items[pg->count++];
                it->kind = LAYOUT_GLANCE;
                if (pg->count == 1) pg->kind = it->kind;
                if (cJSON_IsObject(e)) set_item(it, e);   /* entity, name */
                else copy(it->entity, sizeof(it->entity), ent);
                it->flags = fl;
                const char *ic = cJSON_IsObject(e) ? str(e, "icon") : NULL;
                if (no_icon) copy(it->icon, sizeof(it->icon), "-");
                else if (ic && strlen(ic) < sizeof(it->icon)) copy(it->icon, sizeof(it->icon), ic);
            }
            continue;
        }
        if (ct && !strcmp(ct, "entities")) {   /* one row per entity; dividers, buttons etc. skipped */
            const cJSON *ents = cJSON_GetObjectItemCaseSensitive(card, "entities"), *e;
            const char *title = str(card, "title");
            if (title && !pg->title[0]) {
                copy(pg->title, sizeof(pg->title), title);
                const char *ti = str(card, "icon");
                if (ti && strlen(ti) < sizeof(pg->title_icon)) copy(pg->title_icon, sizeof(pg->title_icon), ti);
            }
            cJSON_ArrayForEach(e, ents) {
                const char *ent = cJSON_IsString(e) ? e->valuestring : cJSON_IsObject(e) ? str(e, "entity") : NULL;
                if (!ent) {
                    const char *rt = cJSON_IsObject(e) ? str(e, "type") : NULL;
                    LOGW("section %d: entities row '%s' not supported, skipped", idx, rt ? rt : "?");
                    continue;
                }
                if (pg->count == LAYOUT_MAX_ROWS) { LOGW("section %d: more than %d cards, extra skipped", idx, LAYOUT_MAX_ROWS); break; }
                layout_item_t *it = &pg->items[pg->count++];
                it->kind = LAYOUT_ENTITIES;
                if (pg->count == 1) pg->kind = it->kind;
                if (cJSON_IsObject(e)) set_item(it, e);   /* entity, name */
                else copy(it->entity, sizeof(it->entity), ent);
                const char *ic = cJSON_IsObject(e) ? str(e, "icon") : NULL;
                if (ic && strlen(ic) < sizeof(it->icon)) copy(it->icon, sizeof(it->icon), ic);
            }
            continue;
        }
        if (ct && !strcmp(ct, "calendar")) {   /* upcoming events of its calendars, one block */
            const cJSON *ents = cJSON_GetObjectItemCaseSensitive(card, "entities"), *e;
            char ids[LAYOUT_CALS_LEN] = "";
            cJSON_ArrayForEach(e, ents) {
                const char *ent = cJSON_IsString(e) ? e->valuestring : cJSON_IsObject(e) ? str(e, "entity") : NULL;
                if (!ent || !ent[0] || strchr(ent, ',') || strchr(ent, '"')) continue;
                size_t n = strlen(ids);
                if (n + strlen(ent) + 2 > sizeof(ids)) { LOGW("section %d: calendar '%s' skipped, too many", idx, ent); continue; }
                if (n) strcat(ids, ",");
                strcat(ids, ent);
            }
            if (!ids[0]) { LOGW("section %d: calendar card without entities, skipped", idx); continue; }
            int k = 0;
            while (k < LAYOUT_MAX_CALS && l->cals[k][0]) k++;
            if (k == LAYOUT_MAX_CALS) { LOGW("section %d: more than %d calendar cards, skipped", idx, LAYOUT_MAX_CALS); continue; }
            if (pg->count == LAYOUT_MAX_ROWS) { LOGW("section %d: more than %d cards, extra skipped", idx, LAYOUT_MAX_ROWS); continue; }
            layout_item_t *it = &pg->items[pg->count++];
            it->kind = LAYOUT_CALENDAR;
            if (pg->count == 1) pg->kind = it->kind;
            copy(l->cals[k], LAYOUT_CALS_LEN, ids);
            it->cal = k + 1;
            size_t first = strcspn(ids, ",");
            if (first < sizeof(it->entity)) { memcpy(it->entity, ids, first); it->entity[first] = 0; }
            const char *title = str(card, "title");
            if (title) copy(it->name, sizeof(it->name), title);
            const char *iv = str(card, "initial_view");
            if (iv && (!strcmp(iv, "listDay") || !strcmp(iv, "dayGridDay"))) it->flags = LAYOUT_CAL_DAY;
            else if (iv && !strcmp(iv, "listWeek")) it->flags = LAYOUT_CAL_WEEK;   /* else month, HA's default */
            continue;
        }
        bool clock = ct && !strcmp(ct, "clock");
        bool gauge = ct && !strcmp(ct, "gauge");
        bool weather = ct && !strcmp(ct, "weather-forecast");   /* current weather + forecast */
        bool thermo = ct && !strcmp(ct, "thermostat");
        bool media = ct && !strcmp(ct, "media-control");
        bool light = ct && !strcmp(ct, "light");
        bool graph = is_graph(card), tile = is_tile(card), button = is_button(card), picture = is_picture(card);
        if (!clock && !graph && !tile && !button && !picture && !weather && !gauge && !thermo && !media && !light) {
            LOGW("section %d: card '%s' not supported, skipped", idx, ct ? ct : "?");
            continue;
        }
        bool src_only = picture && (str(card, "camera_image") || cJSON_GetObjectItemCaseSensitive(card, "image"));
        if (!clock && !src_only && !str(card, "entity")) {   /* a clock shows the board's own time; pictures may have only a source */
            LOGW("section %d: card without entity, skipped", idx);
            continue;
        }
        if (pg->count == LAYOUT_MAX_ROWS) {
            LOGW("section %d: more than %d cards, extra skipped", idx, LAYOUT_MAX_ROWS);
            continue;
        }
        layout_item_t *it = &pg->items[pg->count++];
        it->kind = clock ? LAYOUT_CLOCK : graph ? LAYOUT_GRAPH : button ? LAYOUT_BUTTONS : picture ? LAYOUT_PICTURE : weather ? LAYOUT_WEATHER : gauge ? LAYOUT_GAUGE : thermo ? LAYOUT_THERMOSTAT : media ? LAYOUT_MEDIA : light ? LAYOUT_LIGHT : LAYOUT_VALUES;
        if (pg->count == 1) pg->kind = it->kind;
        if (clock) {
            const char *tf = str(card, "time_format"), *sz = str(card, "clock_size"), *tz = str(card, "time_zone");
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(card, "show_seconds"))) it->flags |= LAYOUT_CLOCK_SECONDS;
            if (tf && !strcmp(tf, "12")) it->flags |= LAYOUT_CLOCK_12H;
            if (sz && !strcmp(sz, "medium")) it->flags |= LAYOUT_CLOCK_MEDIUM;
            else if (sz && !strcmp(sz, "large")) it->flags |= LAYOUT_CLOCK_LARGE;
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(card, "no_background"))) it->flags |= LAYOUT_CLOCK_NO_BG;
            if (tz) copy(it->entity, sizeof(it->entity), tz);
            continue;
        }
        set_item(it, card);
        if (weather) {
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(card, "show_forecast"))) it->flags |= LAYOUT_WX_FORECAST;
            const char *ft = str(card, "forecast_type");
            if (ft && !strcmp(ft, "hourly")) it->flags |= LAYOUT_WX_HOURLY;
        }
        const char *title = picture && !it->name[0] && !it->name_type[0] ? str(card, "title") : NULL;   /* picture-glance */
        if (title) copy(it->name, sizeof(it->name), title);
        if (picture && !it->entity[0] && !it->name[0]) it->flags |= LAYOUT_PIC_NO_NAME;   /* nothing to name it by */
        if (picture && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(card, "show_name"))) it->flags |= LAYOUT_PIC_NO_NAME;
        const char *fit = picture ? str(card, "fit_mode") : NULL;
        if (fit && !strcmp(fit, "cover")) it->flags |= LAYOUT_PIC_COVER;
        else if (fit && !strcmp(fit, "fill")) it->flags |= LAYOUT_PIC_FILL;
        else if (!fit && picture && !strcmp(ct, "picture-glance")) it->flags |= LAYOUT_PIC_COVER;   /* HA fills the card with it */
        const char *src = picture ? str(card, "camera_image") : NULL;   /* HA prefers the camera */
        if (picture && !src) src = str(card, "image");
        const cJSON *img = picture && !src ? cJSON_GetObjectItemCaseSensitive(card, "image") : NULL;
        if (cJSON_IsObject(img)) {         /* UI editor: media selector {media_content_id: ...} */
            src = str(img, "media_content_id");
            const char *ent = src ? strstr(src, "media-source://image/") : NULL;
            if (ent) src = ent + strlen("media-source://image/");   /* an image entity */
        }
        if (src && strlen(src) < LAYOUT_IMAGE_LEN) {
            int k = 0;
            while (k < LAYOUT_MAX_IMAGES && l->images[k][0] && strcmp(l->images[k], src)) k++;
            if (k == LAYOUT_MAX_IMAGES) LOGW("section %d: more than %d picture sources, '%s' skipped", idx, LAYOUT_MAX_IMAGES, src);
            else { copy(l->images[k], LAYOUT_IMAGE_LEN, src); it->image = k + 1; }
        } else if (picture && !src) {
            char *j = cJSON_PrintUnformatted(card);
            LOGW("section %d: picture card without image: %.200s", idx, j ? j : "");
            cJSON_free(j);
        } else if (src) {
            LOGW("section %d: picture source longer than %d characters, skipped", idx, LAYOUT_IMAGE_LEN - 1);
        }
        const char *icon = str(card, "icon");
        if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(card, "show_icon"))) copy(it->icon, sizeof(it->icon), "-");
        else if (icon && strlen(icon) < sizeof(it->icon)) copy(it->icon, sizeof(it->icon), icon);
        if (gauge) {
            it->min = num(card, "min", 0);
            it->max = num(card, "max", 100);
            const cJSON *sv = cJSON_GetObjectItemCaseSensitive(card, "severity");
            static const char *const names[3] = { "green", "yellow", "red" };
            for (int k = 0; k < 3; k++) it->sev[k] = cJSON_IsObject(sv) ? num(sv, names[k], NAN) : NAN;
        }
        if (graph) {
            const cJSON *h = cJSON_GetObjectItemCaseSensitive(card, "hours_to_show");
            int hours = cJSON_IsNumber(h) ? (int)h->valuedouble : 0;
            it->hours = hours < 1 ? LAYOUT_HOURS_DEFAULT : hours > LAYOUT_HOURS_MAX ? LAYOUT_HOURS_MAX : hours;
            const cJSON *lim = cJSON_GetObjectItemCaseSensitive(card, "limits");
            it->min = cJSON_IsObject(lim) ? num(lim, "min", NAN) : NAN;   /* chart floor */
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

static int parse_impl(const char *json, const char *view_path, ha_layout_t *out,
                      layout_view_t *views_out, int max, int *nviews, char *chosen);

int layout_parse(const char *json, const char *view_path, ha_layout_t *out)
{
    return parse_impl(json, view_path, out, NULL, 0, NULL, NULL);
}

int layout_parse_views(const char *json, const char *view_path, ha_layout_t *out,
                       layout_view_t *views, int max, int *nviews, char chosen[32])
{
    *nviews = 0;
    chosen[0] = 0;
    return parse_impl(json, view_path ? view_path : "", out, views, max, nviews, chosen);
}

/* A view's identifier: its path, or its index when it has none */
static void view_id(const cJSON *v, int idx, char out[32])
{
    const char *p = str(v, "path");
    if (p && *p) snprintf(out, 32, "%s", p);
    else snprintf(out, 32, "%d", idx);
}

static int parse_impl(const char *json, const char *view_path, ha_layout_t *out,
                      layout_view_t *views_out, int max, int *nviews, char *chosen)
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
    const cJSON *views = cJSON_GetObjectItemCaseSensitive(cfg, "views"), *view = NULL, *v, *first = NULL;
    layout_view_t *list = views_out;
    (void)max;
    if (cJSON_IsArray(views)) {
        int vi = 0;
        cJSON_ArrayForEach(v, views) {
            const char *p = str(v, "path");
            if (!list) {
                if (p && !strcmp(p, view_path)) { view = v; break; }
                continue;
            }
            if (!cJSON_IsObject(v)) { vi++; continue; }
            char id[32];
            view_id(v, vi, id);
            if (*nviews < max) {
                layout_view_t *o = &list[(*nviews)++];
                snprintf(o->path, sizeof(o->path), "%s", id);
                const char *t = str(v, "title");
                snprintf(o->title, sizeof(o->title), "%s", t && *t ? t : id);
            }
            if (!view && *view_path && !strcmp(id, view_path)) view = v;
            if (!first) first = v;
            vi++;
        }
        if (list && !view) view = first;   /* no choice yet, or the chosen view is gone */
        if (list && view) {
            int i = 0;
            cJSON_ArrayForEach(v, views) {
                if (v == view) { view_id(v, i, chosen); break; }
                i++;
            }
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
                } else if (parse_section(sec, idx, &out->pages[out->count], out)) {
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
        for (int i = 0; i < pg->count; i++)
            if (pg->items[i].kind > LAYOUT_LIGHT) pg->items[i].kind = LAYOUT_VALUES;
        pg->kind = pg->items[0].kind;   /* kind of the first card */
        pg->title[sizeof(pg->title) - 1] = 0;
        for (int i = 0; i < pg->count; i++) {
            pg->items[i].entity[sizeof(pg->items[i].entity) - 1] = 0;
            pg->items[i].name[sizeof(pg->items[i].name) - 1] = 0;
            pg->items[i].name_type[sizeof(pg->items[i].name_type) - 1] = 0;
            pg->items[i].icon[sizeof(pg->items[i].icon) - 1] = 0;
            if (pg->items[i].kind != LAYOUT_PICTURE || pg->items[i].image > LAYOUT_MAX_IMAGES) pg->items[i].image = 0;
            if (pg->items[i].kind != LAYOUT_CALENDAR || pg->items[i].cal > LAYOUT_MAX_CALS) pg->items[i].cal = 0;
            if (pg->items[i].action > LAYOUT_ACT_NONE) pg->items[i].action = LAYOUT_ACT_NONE;
            pg->items[i].flags &= pg->items[i].kind == LAYOUT_PICTURE ? LAYOUT_PIC_FLAGS : pg->items[i].kind == LAYOUT_CLOCK ? LAYOUT_CLOCK_FLAGS
                                : pg->items[i].kind == LAYOUT_GLANCE ? LAYOUT_GLANCE_FLAGS
                                : pg->items[i].kind == LAYOUT_WEATHER ? LAYOUT_WX_FLAGS
                                : pg->items[i].kind == LAYOUT_CALENDAR ? LAYOUT_CAL_FLAGS : 0;
            if (pg->items[i].kind == LAYOUT_GAUGE) {   /* a usable range */
                if (!isfinite(pg->items[i].min)) pg->items[i].min = 0;
                if (!isfinite(pg->items[i].max) || pg->items[i].max <= pg->items[i].min) pg->items[i].max = pg->items[i].min + 100;
            }
            if ((pg->items[i].flags & LAYOUT_CLOCK_SIZE) == LAYOUT_CLOCK_SIZE) pg->items[i].flags &= ~LAYOUT_CLOCK_SIZE;
            uint16_t *h = &pg->items[i].hours;
            if (pg->items[i].kind != LAYOUT_GRAPH) *h = 0;   /* unused; as layout_parse leaves it */
            else if (*h < 1) *h = LAYOUT_HOURS_DEFAULT;
            else if (*h > LAYOUT_HOURS_MAX) *h = LAYOUT_HOURS_MAX;
        }
    }
    for (int k = 0; k < LAYOUT_MAX_IMAGES; k++) l->images[k][LAYOUT_IMAGE_LEN - 1] = 0;
    for (int k = 0; k < LAYOUT_MAX_CALS; k++) l->cals[k][LAYOUT_CALS_LEN - 1] = 0;
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
            if (pg.items[i].kind == LAYOUT_CLOCK || (pg.items[i].image && !pg.items[i].entity[0]) ||   /* nothing to check */
                keep(pg.items[i].entity, pg.items[i].image && pg.items[i].kind == LAYOUT_PICTURE ? LAYOUT_VALUES : pg.items[i].kind, ctx)) pg.items[n++] = pg.items[i];
        for (int i = n; i < LAYOUT_MAX_ROWS; i++) memset(&pg.items[i], 0, sizeof(layout_item_t));
        pg.count = n;
        if (n) { pg.kind = pg.items[0].kind; l->pages[np++] = pg; }
    }
    for (int p = np; p < LAYOUT_MAX_PAGES; p++) memset(&l->pages[p], 0, sizeof(layout_page_t));
    l->count = np;
    return np;
}

int layout_parse_dashboards(const char *msg, const char *cur, layout_view_t *out, int max, int *current)
{
    int n = 0;
    *current = -1;
    cJSON *root = msg ? cJSON_Parse(msg) : NULL;
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "result"), *d;
    if (cJSON_IsArray(list)) cJSON_ArrayForEach(d, list) {
        if (n >= max) break;
        const char *path = str(d, "url_path"), *title = str(d, "title");
        if (!path || !path[0]) continue;
        layout_view_t *v = &out[n];
        memset(v, 0, sizeof(*v));
        snprintf(v->path, sizeof(v->path), "%s", path);
        snprintf(v->title, sizeof(v->title), "%s", title && title[0] ? title : path);
        if (cur && !strcmp(v->path, cur)) *current = n;
        n++;
    }
    cJSON_Delete(root);
    return n;
}
