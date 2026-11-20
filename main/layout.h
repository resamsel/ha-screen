/* Main-screen layout read from a Home Assistant dashboard view (lovelace/config). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LAYOUT_MAX_PAGES 12
#define LAYOUT_MAX_ROWS  4
#define LAYOUT_HOURS_DEFAULT 24
#define LAYOUT_HOURS_MAX     168
#define LAYOUT_MAX_IMAGES    4      /* picture cards with an image:/camera_image: source */
#define LAYOUT_IMAGE_LEN     128

typedef enum { LAYOUT_VALUES, LAYOUT_GRAPH, LAYOUT_BUTTONS, LAYOUT_CLOCK, LAYOUT_PICTURE, LAYOUT_WEATHER } layout_kind_t;

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

/* what a tap on a button does: toggle/run/activate, or nothing (other tap_action types) */
typedef enum { LAYOUT_ACT_TOGGLE, LAYOUT_ACT_NONE } layout_action_t;

typedef struct {
    char entity[64];        /* clock pages: the card's time_zone (IANA name), "" = default */
    char name[32];          /* plain card name; "" = resolve name_type, else friendly_name */
    uint8_t action;         /* layout_action_t, button pages */
    uint16_t hours;         /* graph span (graph pages) */
    uint8_t flags;          /* LAYOUT_CLOCK_* (clock cards), LAYOUT_PIC_* (pictures) */
    uint8_t kind;           /* layout_kind_t of this card: each card is its own block */
    uint8_t image;          /* pictures: 1-based index into ha_layout_t.images, 0 = the entity itself */
    char name_type[48];     /* card name as HA name parts, '|'-separated: floor, area, device,
                             * entity, text:<text>; resolved by the network task */
} layout_item_t;

typedef struct {
    layout_kind_t kind;     /* kind of the first card (pages may mix kinds: see items[].kind) */
    char title[32];         /* section heading, "" if none */
    int count;
    layout_item_t items[LAYOUT_MAX_ROWS];
} layout_page_t;

typedef struct {
    int count;
    layout_page_t pages[LAYOUT_MAX_PAGES];
    /* picture sources: a camera entity id (camera_image:) or an image URL (image:),
     * absolute or relative to HA */
    char images[LAYOUT_MAX_IMAGES][LAYOUT_IMAGE_LEN];
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

/* Removes items whose entity keep() rejects (pictures with their own image source are
 * checked as LAYOUT_VALUES: the entity only has to exist), and pages left empty. Returns the page count. */
int layout_filter(ha_layout_t *l, bool (*keep)(const char *entity, layout_kind_t kind, void *ctx), void *ctx);
