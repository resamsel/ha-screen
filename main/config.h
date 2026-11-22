/* Non-secret device configuration. Wi-Fi credentials, the HA URL and the HA token are kept
 * in NVS and set in the setup portal (provision.h); build-time values are only defaults. */
#pragma once

/* Default Home Assistant base URL (no trailing slash) until the setup portal stores one */
#define HA_URL_DEFAULT       "http://homeassistant.local:8123"

/* Setup portal (provision.h): opened at boot when settings are missing, when the BOOT
 * button is held while powering on, or when Wi-Fi or HA (unreachable, token rejected) didn't
 * work for this many seconds since boot; the form then says why.
 * With settings stored it gives up after HA_PORTAL_IDLE_S without a phone connected. */
#define HA_WIFI_PORTAL_S     120
#define HA_PORTAL_IDLE_S     300

/* How often the display reads all data from Home Assistant (entities and the graph
 * history), in milliseconds */
#define HA_POLL_INTERVAL_MS  60000

/* Live updates: between HTTP rounds a websocket subscription delivers state changes at
 * once; the HTTP round (graph history, layout, a full resync) then runs this often */
#define HA_LIVE_RESYNC_MS    (5 * 60 * 1000)

/* HTTP requests to HA: timeout per attempt, extra attempts after a failure, and the pause
 * before retry n (n * HA_HTTP_RETRY_MS) */
#define HA_HTTP_TIMEOUT_MS   5000
#define HA_HTTP_RETRIES      2
#define HA_HTTP_RETRY_MS     500

/* Board helpers (issue #24): each board's ID comes from its MAC (screen_a1b2c3); its name
 * prefixes the board's helpers; the HA helper input_text.<id> can hold the name. Name kitchen:
 *   input_boolean.kitchen_theme         on = dark theme, off = light (the button)
 *   input_boolean.kitchen_landscape     on = landscape, off = portrait (Settings)
 *   input_boolean.kitchen_presentation  presentation mode on/off
 *   input_number.kitchen_presentation_interval  seconds per page
 *   input_number.kitchen_brightness     backlight (the slider; uses the entity's min/max)
 *   input_text.kitchen_dashboard / _view, input_number.kitchen_page  (see below)
 * The name is set in the setup portal (board ID if none); the helper, when it exists, overrides
 * it. Every helper is optional: without one the board keeps that setting itself (NVS), and
 * creating it later hands the setting to HA. */

/* The main-screen pages come from the dashboard and view chosen on the board (config panel,
 * first page; asked for once the board reaches HA): each section of the view is one page
 * (tiles = value rows, a line-graph sensor card = graph page). Re-read every interval. */
#define HA_LAYOUT_INTERVAL_MS 60000

/* Shown dashboard, view and page, kept in sync both ways (the last change wins; flash keeps
 * them when HA is unreachable). Optional: missing or unavailable entities are ignored.
 * input_text: dashboard url_path ("" = none chosen yet) and view path ("" = the first view);
 * input_number: page, counted from 1 (not written while presentation mode is on). */

/* Picture cards (picture-entity, camera.* / image.*): refresh interval while the page is shown */
#define HA_PICTURE_INTERVAL_MS 60000

/* Card data (values, graphs, buttons, weather, icons) is saved to flash so the board shows it
 * right after booting: first a minute after start, then at most this often (flash wear) */
#define HA_CACHE_INTERVAL_MS (15 * 60 * 1000)

/* Graph pages (the time span comes from the dashboard card's hours_to_show) */
/* Line smoothing: Gaussian blur width in points (pixels); 0 = raw data, higher = smoother.
 * Write decimals with a point (3.0f or 3, not 3f). */
#define HA_GRAPH_SMOOTHING   6.0f
#define HA_GRAPH_LINE_WIDTH  4       /* graph line thickness in pixels */
#define HA_GRAPH_POINTS      168     /* horizontal resolution (1 px per point) */

/* A tapped value (with its unit and name) grows by this factor; 1.2 = 20 % bigger */
#define HA_ACTIVE_SCALE      1.6f

/* Local time zone for the graph's time labels (POSIX TZ string; Vienna = CET/CEST) */
#define HA_TIMEZONE          "CET-1CEST,M3.5.0,M10.5.0/3"

/* Device diagnostics posted to HA as sensor.<board name>_wifi_rssi, _free_heap,
 * _largest_free_block, _uptime and _firmware (version, reset reason); errors go to HA's
 * logbook on sensor.<board name>_firmware (show them with a Logbook card on that entity) */
#define HA_DIAG_INTERVAL_MS  (5 * 60 * 1000)
