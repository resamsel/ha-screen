/* Main-screen layout read from a Home Assistant dashboard view (lovelace/config). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LAYOUT_MAX_PAGES 12
#define LAYOUT_MAX_ROWS  4
#define LAYOUT_HOURS_DEFAULT 24
#define LAYOUT_HOURS_MAX     168

typedef enum { LAYOUT_VALUES, LAYOUT_GRAPH, LAYOUT_BUTTONS } layout_kind_t;

/* what a tap on a button does: toggle/run/activate, or nothing (other tap_action types) */
typedef enum { LAYOUT_ACT_TOGGLE, LAYOUT_ACT_NONE } layout_action_t;

typedef struct {
    char entity[64];
    char name[32];          /* plain card name; "" = resolve name_type, else friendly_name */
    uint8_t action;         /* layout_action_t, button pages */
    char name_type[48];     /* card name as HA name parts, '|'-separated: floor, area, device,
                             * entity, text:<text>; resolved by the network task */
} layout_item_t;

typedef struct {
    layout_kind_t kind;
    char title[32];         /* section heading, "" if none */
    int hours;              /* graph span (graph pages) */
    int count;
    layout_item_t items[LAYOUT_MAX_ROWS];
} layout_page_t;

typedef struct {
    int count;
    layout_page_t pages[LAYOUT_MAX_PAGES];
} ha_layout_t;

/* json: full websocket result message ({"result":{...}}) or the bare dashboard config.
 * Unknown or unusable parts are skipped (logged). Zeroes *out first.
 * Returns the number of pages; 0 = nothing usable. */
int layout_parse(const char *json, const char *view_path, ha_layout_t *out);

/* FNV-1a over the whole struct; layouts from layout_parse compare equal when identical. */
uint32_t layout_hash(const ha_layout_t *l);

/* Jinja template that renders the item's name_type with HA's /api/template.
 * Returns false when name_type holds nothing usable. */
bool layout_name_template(const layout_item_t *it, char *out, int n);

/* Repairs a layout from an untrusted source (NVS): clamps hours and kind, terminates strings.
 * Returns false (and zeroes *l) when counts are out of range. */
bool layout_sanitize(ha_layout_t *l);

/* Removes items whose entity keep() rejects, and pages left empty. Returns the page count. */
int layout_filter(ha_layout_t *l, bool (*keep)(const char *entity, layout_kind_t kind, void *ctx), void *ctx);
