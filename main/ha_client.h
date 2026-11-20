#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "layout.h"

typedef enum {
    HA_STATE_OFFLINE,  /* no Wi-Fi */
    HA_STATE_UNKNOWN,  /* reachable, state not on/off */
    HA_STATE_ERROR,    /* HA unreachable or rejected the request */
    HA_STATE_OFF,
    HA_STATE_ON,
} ha_state_t;

/* Called from the network task whenever the entity state is (re)read. */
typedef void (*ha_state_cb_t)(ha_state_t state);

/* Called when the brightness entity changes: value and the entity's min/max. */
typedef void (*ha_brightness_cb_t)(float value, float min, float max);

/* Called after a toggle/set request finished and the state was re-read; ok = HA accepted it. */
typedef void (*ha_done_cb_t)(bool ok);

/* New main-screen layout (from the dashboard or the NVS cache). The pointer is only valid
 * during the call. Called from the network task before any sensor/graph callback for it. */
typedef void (*ha_layout_cb_t)(const ha_layout_t *layout);
void ha_client_set_layout_cb(ha_layout_cb_t cb);  /* call before ha_client_start */

/* Button page row: state for display; name = card name or friendly_name */
typedef enum { HA_BTN_UNAVAILABLE, HA_BTN_OFF, HA_BTN_ON, HA_BTN_SCRIPT, HA_BTN_SCENE, HA_BTN_PRESS } ha_btn_state_t;   /* PRESS: button / input_button */
typedef void (*ha_button_cb_t)(int page, int row, ha_btn_state_t state, const char *name);
void ha_client_set_button_cb(ha_button_cb_t cb);  /* call before ha_client_start */

/* Tap on a button: toggle / run / activate its entity. The result comes back as press_cb. */
typedef void (*ha_press_cb_t)(int page, int row, bool ok);
void ha_client_set_press_cb(ha_press_cb_t cb);    /* call before ha_client_start */
void ha_client_press(int page, int row);

/* true while the network task talks to HA (a poll, a tap, the dashboard check), then false */
typedef void (*ha_busy_cb_t)(bool busy);
void ha_client_set_busy_cb(ha_busy_cb_t cb);      /* call before ha_client_start */

/* Value page row: page/row index into the current layout; name = card name or friendly_name. */
typedef void (*ha_sensor_cb_t)(int page, int row, const char *value, const char *unit, const char *name);

/* Graph `row` on graph page `page`: `latest`/`unit`/`name` = current value (points NULL), or `points` = n history
 * buckets oldest first, NAN where no data yet (latest NULL), with `end` = the UTC time (s) of
 * the right edge; the left edge is `hours` earlier. zero_based = y axis starts at 0. */
typedef void (*ha_graph_cb_t)(int page, int row, const char *latest, const char *unit, const char *name,
                              const float *points, int n, int64_t end, int hours, bool zero_based);

/* Called after every poll with the errors it hit, one per line; "" = none. */
typedef void (*ha_error_cb_t)(const char *errors);
void ha_client_set_error_cb(ha_error_cb_t cb);  /* call before ha_client_start */

/* Called after every poll with the orientation entity's state (on = landscape). */
typedef void (*ha_landscape_cb_t)(bool landscape);
void ha_client_set_landscape_cb(ha_landscape_cb_t cb);  /* call before ha_client_start */
void ha_client_toggle_landscape(void);

/* Picture card (picture-entity): pixels = w*h RGB565 in LVGL's byte order (LV_COLOR_16_SWAP),
 * the whole image scaled to fit w x h of the requested size; name = card name or friendly_name.
 * zoom 2: decoded at half size for lack of memory, show it zoomed 2x.
 * The callback owns pixels and must free() them. pixels NULL = drop the current picture
 * (to make room for the next one when memory is short). */
typedef void (*ha_picture_cb_t)(int page, int row, uint16_t *pixels, int w, int h, int zoom, const char *name);
void ha_client_set_picture_cb(ha_picture_cb_t cb);  /* call before ha_client_start */

/* Icon of a value, graph or button card: "mdi:<name>", or "" for none. Sent when it changes
 * (and again after every new layout). */
typedef void (*ha_icon_cb_t)(int page, int row, const char *icon);
void ha_client_set_icon_cb(ha_icon_cb_t cb);  /* call before ha_client_start */

/* Weather card: condition = HA state (sunny, fog, ...; "" when unavailable), temperature
 * and humidity as HA sends them ("" when missing), unit like "°C". */
typedef void (*ha_weather_cb_t)(int page, int row, const char *condition, const char *temp, const char *unit,
                                const char *humidity, const char *name);
void ha_client_set_weather_cb(ha_weather_cb_t cb);  /* call before ha_client_start */

/* The page now on screen and the size of each of its picture blocks (0 = no picture).
 * Its pictures are fetched at once, then every HA_PICTURE_INTERVAL_MS while it stays shown. */
void ha_client_show_page(int page, const uint16_t w[LAYOUT_MAX_ROWS], const uint16_t h[LAYOUT_MAX_ROWS]);

void ha_client_start(ha_state_cb_t cb, ha_brightness_cb_t bcb, ha_done_cb_t dcb, ha_sensor_cb_t scb,
                     ha_graph_cb_t gcb);
void ha_client_toggle(void);
void ha_client_set_brightness(float value);
void ha_client_refresh(void);  /* re-read all entities now; ends with the done callback */
