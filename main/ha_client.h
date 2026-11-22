#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "layout.h"
#include "cal_parse.h"

typedef enum {
    HA_STATE_OFFLINE,  /* no Wi-Fi */
    HA_STATE_UNKNOWN,  /* reachable, state not on/off */
    HA_STATE_LOCAL,    /* reachable, no theme helper: the board keeps its own theme */
    HA_STATE_ERROR,    /* HA unreachable or rejected the request */
    HA_STATE_OFF,
    HA_STATE_ON,
} ha_state_t;

/* Called from the network task whenever the entity state is (re)read. */
typedef void (*ha_state_cb_t)(ha_state_t state);

/* Called when the brightness entity changes: value and the entity's min/max. */
typedef void (*ha_brightness_cb_t)(float value, float min, float max);

/* Called after a set request or a refresh finished and the state was re-read; ok = HA
 * accepted it; refresh = this round re-read the dashboard (refresh button, view change). */
typedef void (*ha_done_cb_t)(bool ok, bool refresh);

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

/* Board name (issue #24): name_entity is the helper to create and set while the board has
 * no name (show the setup screen), NULL once named; name = the board's name or "" */
typedef void (*ha_setup_cb_t)(const char *name_entity, const char *name);
void ha_client_set_setup_cb(ha_setup_cb_t cb);  /* call before ha_client_start */

/* The board's own settings, for helpers created after setup (they start with these values).
 * Called from the network task. */
typedef struct {
    bool dark, landscape, pres_on;
    int pres_secs, bri_pct, page;      /* page counted from 0 */
    bool night;
    int night_pct, timeout_min;        /* night brightness %, screen timeout in minutes */
    char dash[32], view[32];
} ha_local_t;
typedef void (*ha_local_cb_t)(ha_local_t *out);
void ha_client_set_local_cb(ha_local_cb_t cb);  /* call before ha_client_start */

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
void ha_client_set_landscape(bool landscape);
/* Presentation mode: called after every poll with HA's on/off and interval (seconds; 0 = unknown).
 * ha_client_set_presentation writes both to HA. */
typedef void (*ha_pres_cb_t)(bool on, int secs);
void ha_client_set_pres_cb(ha_pres_cb_t cb);  /* call before ha_client_start */
void ha_client_set_presentation(bool on, int secs);

/* Night mode (issue #17): input_boolean.<name>_night, input_number.<name>_night_brightness (%),
 * input_number.<name>_screen_timeout (minutes, 0 = never). The callback gets -1 for a value
 * whose helper HA doesn't have (the board keeps its own); ha_client_set_night writes them. */
typedef void (*ha_night_cb_t)(int night, int night_pct, int timeout_min);
void ha_client_set_night_cb(ha_night_cb_t cb);  /* call before ha_client_start */
void ha_client_set_night(bool night, int night_pct, int timeout_min);

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

/* Weather forecast (cards with show_forecast: true): up to HA_FORECAST_MAX entries from
 * weather.get_forecasts. t = UTC seconds of the entry; hi/lo as HA sends them ("" if missing). */
#define HA_FORECAST_MAX 8
typedef struct { int64_t t; char cond[16]; char hi[8]; char lo[8]; } ha_forecast_t;
typedef void (*ha_forecast_cb_t)(int page, int row, const ha_forecast_t *f, int n, bool hourly);
void ha_client_set_forecast_cb(ha_forecast_cb_t cb);  /* call before ha_client_start */

/* Thermostat card (climate entity), values as HA sends them ("" when missing): mode = the
 * state (heat, off, ...), action = hvac_action (heating, idle, ...), step = target_temp_step */
typedef struct { char mode[16], action[16], cur[12], target[12], min[12], max[12], step[8]; } ha_climate_t;
typedef void (*ha_climate_cb_t)(int page, int row, const ha_climate_t *c, const char *name);
void ha_client_set_climate_cb(ha_climate_cb_t cb);  /* call before ha_client_start */
/* New target temperature for thermostat `row` on `page` (climate.set_temperature) */
void ha_client_set_target(int page, int row, float target);

/* Media control card: state (playing, paused, idle, off, ...), media_title, media_artist */
typedef void (*ha_media_cb_t)(int page, int row, const char *state, const char *title, const char *artist, const char *name);
void ha_client_set_media_cb(ha_media_cb_t cb);  /* call before ha_client_start */
typedef enum { HA_MEDIA_PLAY_PAUSE, HA_MEDIA_NEXT } ha_media_cmd_t;
void ha_client_media(int page, int row, ha_media_cmd_t cmd);

/* Light card: state (on, off, unavailable, ...), brightness 0-100 % (-1 = none, e.g. off
 * or a light without dimming) */
typedef void (*ha_light_cb_t)(int page, int row, const char *state, int pct, const char *name);
void ha_client_set_light_cb(ha_light_cb_t cb);  /* call before ha_client_start */
/* pct 1-100: light.turn_on at that brightness; 0: turn off; -1: toggle */
void ha_client_light(int page, int row, int pct);

/* Calendar card: up to CAL_MAX_EVENTS events of its calendars (calendar.get_events), earliest
 * first: today's (day view), the next 7 days' (list view) or the month's (month view: also
 * mask, bit d-1 = an event on day d of the month starting at month, local midnight UTC s). */
typedef void (*ha_calendar_cb_t)(int page, int row, const cal_event_t *ev, int n, uint32_t mask, int64_t month);
void ha_client_set_calendar_cb(ha_calendar_cb_t cb);  /* call before ha_client_start */

/* The page now on screen and the size of each of its picture blocks (0 = no picture).
 * Its pictures are fetched at once, then every HA_PICTURE_INTERVAL_MS while it stays shown. */
void ha_client_show_page(int page, const uint16_t w[LAYOUT_MAX_ROWS], const uint16_t h[LAYOUT_MAX_ROWS]);

void ha_client_start(ha_state_cb_t cb, ha_brightness_cb_t bcb, ha_done_cb_t dcb, ha_sensor_cb_t scb,
                     ha_graph_cb_t gcb);
void ha_client_set_theme(bool dark);   /* theme entity on = dark */
/* The last data of every card, theme and brightness again through their callbacks (after a
 * UI rebuild). Any task; the callbacks take the LVGL lock themselves. */
void ha_client_replay(void);
void ha_client_set_brightness(float value);
void ha_client_refresh(void);  /* re-read all entities now; ends with the done callback */

/* Dashboard views, after every dashboard read: n views, current = index of the one shown
 * (-1 if none). The array is only valid during the call. */
typedef void (*ha_views_cb_t)(const layout_view_t *views, int n, int current);
void ha_client_set_views_cb(ha_views_cb_t cb);  /* call before ha_client_start */
/* Show this view's pages ("" = the first view): re-reads the dashboard like ha_client_refresh.
 * May be called before ha_client_start to set the view to start with. */
void ha_client_set_view(const char *path);
/* The HA dashboards (lovelace/dashboards/list) after every dashboard read; current = the one
 * shown (-1 when it isn't in the list or none is chosen yet). */
typedef void (*ha_dashboards_cb_t)(const layout_view_t *dashes, int n, int current);
void ha_client_set_dashboards_cb(ha_dashboards_cb_t cb);  /* call before ha_client_start */
/* Show this dashboard's views (url_path; "" = none chosen yet), starting with its view `view`
 * ("" = the first): re-reads the dashboard like ha_client_refresh. */
void ha_client_set_dashboard(const char *url_path, const char *view);

/* The dashboard / view / page helpers as read from HA (poll or live): dash and
 * view NULL when their entity is missing or unavailable, page (0-based) -1 then. Not called
 * while a write from ha_client_set_nav is under way. */
typedef void (*ha_nav_cb_t)(const char *dash, const char *view, int page);
void ha_client_set_nav_cb(ha_nav_cb_t cb);  /* call before ha_client_start */
/* Writes the shown dashboard, view and page (0-based; -1 = leave the page entity alone) */
void ha_client_set_nav(const char *dash, const char *view, int page);
