/* Main-screen layout read from a Home Assistant dashboard view (lovelace/config). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LAYOUT_MAX_PAGES 12
#define LAYOUT_MAX_ROWS  4
#define LAYOUT_HOURS_DEFAULT 24
#define LAYOUT_HOURS_MAX     168
#define LAYOUT_STATS_HOURS_MAX (31 * 24)   /* statistics-graph: up to a month of long-term statistics */
#define LAYOUT_MAX_IMAGES    4      /* picture cards with an image:/camera_image: source */
#define LAYOUT_IMAGE_LEN     128
#define LAYOUT_MAX_CALS      4      /* calendar cards */
#define LAYOUT_CALS_LEN      192    /* a calendar card's calendar.* ids, comma-separated */

typedef enum { LAYOUT_VALUES, LAYOUT_GRAPH, LAYOUT_BUTTONS, LAYOUT_CLOCK, LAYOUT_PICTURE, LAYOUT_WEATHER, LAYOUT_GAUGE, LAYOUT_GLANCE, LAYOUT_CALENDAR,
               LAYOUT_ENTITIES /* entities card: one item (row) per entity */,
               LAYOUT_THERMOSTAT, LAYOUT_MEDIA, LAYOUT_LIGHT } layout_kind_t;

/* clock card options (layout_item_t.flags) */
#define LAYOUT_CLOCK_SECONDS 0x01
#define LAYOUT_CLOCK_12H     0x02
#define LAYOUT_CLOCK_SIZE    0x0C   /* clock_size: */
#define LAYOUT_CLOCK_SMALL   0x00
#define LAYOUT_CLOCK_MEDIUM  0x04
#define LAYOUT_CLOCK_LARGE   0x08
#define LAYOUT_CLOCK_NO_BG   0x10   /* no_background */
#define LAYOUT_CLOCK_FLAGS   0x1F

/* picture-entity card options (layout_item_t.flags) */
#define LAYOUT_PIC_NO_NAME   0x01   /* show_name: false */
#define LAYOUT_PIC_COVER     0x02   /* fit_mode: cover (fill the block, crop the overflow) */
#define LAYOUT_PIC_FILL      0x04   /* fit_mode: fill (stretch to the block) */
#define LAYOUT_PIC_FLAGS     0x07   /* default fit_mode contain: whole image, aspect kept */

/* weather-forecast card options (layout_item_t.flags) */
#define LAYOUT_WX_FORECAST     0x01   /* show_forecast: true (off unless set) */
#define LAYOUT_WX_HOURLY       0x02   /* forecast_type: hourly (else daily) */
#define LAYOUT_WX_FLAGS        0x03

/* graph options (layout_item_t.flags): statistics-graph reads long-term statistics (issue #52) */
#define LAYOUT_GRAPH_STATS     0x01
#define LAYOUT_GRAPH_PERIOD    0x0E   /* period: */
#define LAYOUT_GRAPH_HOUR      0x00   /* HA's default */
#define LAYOUT_GRAPH_5MIN      0x02
#define LAYOUT_GRAPH_DAY       0x04
#define LAYOUT_GRAPH_WEEK      0x06
#define LAYOUT_GRAPH_MONTH     0x08
#define LAYOUT_GRAPH_TYPE      0x30   /* the statistic drawn: */
#define LAYOUT_GRAPH_MEAN      0x00
#define LAYOUT_GRAPH_MIN       0x10
#define LAYOUT_GRAPH_MAX       0x20
#define LAYOUT_GRAPH_STATE     0x30   /* total-type sensors (energy, ...) */
#define LAYOUT_GRAPH_FLAGS     0x3F

/* glance card options (layout_item_t.flags), one item per entity of the card */
#define LAYOUT_GLANCE_NO_NAME  0x01   /* show_name: false */
#define LAYOUT_GLANCE_NO_STATE 0x02   /* show_state: false */
#define LAYOUT_GLANCE_FLAGS    0x03

/* calendar card options (layout_item_t.flags): the card's initial_view */
#define LAYOUT_CAL_SPAN       0x03
#define LAYOUT_CAL_MONTH      0x00   /* dayGridMonth (HA's default): month grid, days with events marked */
#define LAYOUT_CAL_DAY        0x01   /* dayGridDay / listDay: today's events */
#define LAYOUT_CAL_WEEK       0x02   /* listWeek: the next 7 days' events, by day */
#define LAYOUT_CAL_FLAGS      0x03

/* what a tap on a button does: toggle/run/activate, or nothing (other tap_action types) */
typedef enum { LAYOUT_ACT_TOGGLE, LAYOUT_ACT_NONE } layout_action_t;

typedef struct {
    char entity[64];        /* clock pages: the card's time_zone (IANA name), "" = default */
    char name[32];          /* plain card name; "" = resolve name_type, else friendly_name */
    uint8_t action;         /* layout_action_t, button pages */
    uint16_t hours;         /* graph span (graph pages) */
    float min, max;         /* gauge range (default 0..100); graphs: min = limits.min of the
                             * card (y axis floor), NAN = the data's smallest value */
    float sev[3];           /* gauge severity: green, yellow, red from these values on; NAN = none */
    uint8_t flags;          /* LAYOUT_CLOCK_* (clock cards), LAYOUT_PIC_* (pictures) */
    uint8_t kind;           /* layout_kind_t of this card: each card is its own block */
    uint8_t image;          /* pictures: 1-based index into ha_layout_t.images, 0 = the entity itself */
    uint8_t cal;            /* calendars: 1-based index into ha_layout_t.cals (entity = the first one) */
    char icon[40];          /* card icon ("mdi:..."); "" = the entity's own; "-" = none (show_icon: false) */
    char name_type[48];     /* card name as HA name parts, '|'-separated: floor, area, device,
                             * entity, text:<text>; resolved by the network task */
} layout_item_t;

typedef struct {
    layout_kind_t kind;     /* kind of the first card (pages may mix kinds: see items[].kind) */
    char title[32];         /* section heading, "" if none */
    char title_icon[40];    /* the heading's icon ("mdi:..."), "" if none */
    int count;
    layout_item_t items[LAYOUT_MAX_ROWS];
} layout_page_t;

typedef struct {
    int count;
    layout_page_t pages[LAYOUT_MAX_PAGES];
    /* picture sources: a camera entity id (camera_image:) or an image URL (image:),
     * absolute or relative to HA */
    char images[LAYOUT_MAX_IMAGES][LAYOUT_IMAGE_LEN];
    char cals[LAYOUT_MAX_CALS][LAYOUT_CALS_LEN];   /* calendar cards' entity lists */
} ha_layout_t;

/* json: full websocket result message ({"result":{...}}) or the bare dashboard config.
 * Unknown or unusable parts are skipped (logged). Zeroes *out first.
 * Returns the number of pages; 0 = nothing usable. */
int layout_parse(const char *json, const char *view_path, ha_layout_t *out);

/* A dashboard view, as offered in the config panel's view selector */
#define LAYOUT_MAX_VIEWS 8
typedef struct {
    char path[32];          /* the view's path, or its index ("0", "1", ...) when it has none */
    char title[32];         /* its title, else the path */
} layout_view_t;

/* layout_parse that also lists the dashboard's views (up to max) in views / *nviews.
 * view_path "" or not found picks the first view; *chosen gets the path used ("" if none). */
int layout_parse_views(const char *json, const char *view_path, ha_layout_t *out,
                       layout_view_t *views, int max, int *nviews, char chosen[32]);

/* lovelace/dashboards/list result message -> dashboards (path = url_path, title, else the
 * path), up to max. *current = index of url_path `cur` (-1 if not listed). Returns the count. */
int layout_parse_dashboards(const char *msg, const char *cur, layout_view_t *out, int max, int *current);

/* FNV-1a over the whole struct; layouts from layout_parse compare equal when identical. */
uint32_t layout_hash(const ha_layout_t *l);

/* Jinja template that renders the item's name_type with HA's /api/template.
 * Returns false when name_type holds nothing usable. */
bool layout_name_template(const layout_item_t *it, char *out, int n);

/* Repairs a layout from an untrusted source (NVS): clamps hours and kind, terminates strings.
 * Returns false (and zeroes *l) when counts are out of range. */
bool layout_sanitize(ha_layout_t *l);

/* Removes items whose entity keep() rejects (pictures with their own image source are
 * checked as LAYOUT_VALUES: the entity only has to exist), and pages left empty. Returns the page count. */
int layout_filter(ha_layout_t *l, bool (*keep)(const char *entity, layout_kind_t kind, void *ctx), void *ctx);
