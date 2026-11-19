#pragma once
#include <stdbool.h>
#include <stdint.h>

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

/* Called for each sensor with display text: page 0 = temperature, 1 = humidity;
 * row indexes into that page's entity list in config.h. */
typedef void (*ha_sensor_cb_t)(int page, int row, const char *value, const char *unit);

/* Graph page: `latest`/`unit` = current value (points NULL), or `points` = n history
 * buckets oldest first, NAN where no data yet (latest NULL), with `end` = the UTC
 * time (s) of the right edge; the left edge is HA_GRAPH_HOURS earlier. */
typedef void (*ha_graph_cb_t)(int graph, const char *latest, const char *unit, const float *points, int n,
                              int64_t end);

/* Called after every poll with the errors it hit, one per line; "" = none. */
typedef void (*ha_error_cb_t)(const char *errors);
void ha_client_set_error_cb(ha_error_cb_t cb);  /* call before ha_client_start */

/* Called after every poll with the orientation entity's state (on = landscape). */
typedef void (*ha_landscape_cb_t)(bool landscape);
void ha_client_set_landscape_cb(ha_landscape_cb_t cb);  /* call before ha_client_start */
void ha_client_toggle_landscape(void);

void ha_client_start(ha_state_cb_t cb, ha_brightness_cb_t bcb, ha_done_cb_t dcb, ha_sensor_cb_t scb,
                     ha_graph_cb_t gcb);
void ha_client_toggle(void);
void ha_client_set_brightness(float value);
void ha_client_refresh(void);  /* re-read all entities now; ends with the done callback */
