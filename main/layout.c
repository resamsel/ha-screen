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

static bool is_tile(const cJSON *card)
{
    const char *type = str(card, "type");
    return type && !strcmp(type, "tile");
}

static void set_item(layout_item_t *it, const cJSON *card)
{
    copy(it->entity, sizeof(it->entity), str(card, "entity"));
    const char *name = str(card, "name");  /* objects like {"type":"device"} are ignored */
    copy(it->name, sizeof(it->name), name ? name : "");
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
        bool graph = is_graph(card), tile = is_tile(card);
        if (!graph && !tile) {
            LOGW("section %d: card '%s' not supported, skipped", idx, ct ? ct : "?");
            continue;
        }
        if (!str(card, "entity")) {
            LOGW("section %d: card without entity, skipped", idx);
            continue;
        }
        if (!kind_set) {
            pg->kind = graph ? LAYOUT_GRAPH : LAYOUT_VALUES;
            kind_set = true;
        }
        if (pg->kind == LAYOUT_GRAPH) {
            if (!graph || pg->count) {
                LOGW("section %d: graph page uses only its first graph, extra card skipped", idx);
                continue;
            }
            const cJSON *h = cJSON_GetObjectItemCaseSensitive(card, "hours_to_show");
            int hours = cJSON_IsNumber(h) ? (int)h->valuedouble : 0;
            pg->hours = hours < 1 ? LAYOUT_HOURS_DEFAULT : hours > LAYOUT_HOURS_MAX ? LAYOUT_HOURS_MAX : hours;
        } else {
            if (!tile) {
                LOGW("section %d: graph in a tile section skipped", idx);
                continue;
            }
            if (pg->count == LAYOUT_MAX_ROWS) {
                LOGW("section %d: more than %d tiles, extra skipped", idx, LAYOUT_MAX_ROWS);
                continue;
            }
        }
        set_item(&pg->items[pg->count++], card);
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

int layout_filter(ha_layout_t *l, bool (*keep)(const char *entity, void *ctx), void *ctx)
{
    int np = 0;
    for (int p = 0; p < l->count; p++) {
        layout_page_t pg = l->pages[p];
        int n = 0;
        for (int i = 0; i < pg.count; i++)
            if (keep(pg.items[i].entity, ctx)) pg.items[n++] = pg.items[i];
        for (int i = n; i < LAYOUT_MAX_ROWS; i++) memset(&pg.items[i], 0, sizeof(layout_item_t));
        pg.count = n;
        if (n) l->pages[np++] = pg;
    }
    for (int p = np; p < LAYOUT_MAX_PAGES; p++) memset(&l->pages[p], 0, sizeof(layout_page_t));
    l->count = np;
    return np;
}
