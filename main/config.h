/* Non-secret device configuration. Wi-Fi credentials and the HA token come from the
 * WIFI_SSID, WIFI_PASS and HA_TOKEN environment variables at build time. */
#pragma once

/* Home Assistant base URL, no trailing slash */
#define HA_URL               "https://home.panzar.io"

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

/* Board helpers (issue #24): each board's ID comes from its MAC (screen_a1b2c3); the HA
 * helper input_text.<id> holds its name, which prefixes the board's helpers. Name kitchen:
 *   input_boolean.kitchen_theme         on = dark theme, off = light (the button)
 *   input_boolean.kitchen_landscape     on = landscape, off = portrait (Settings)
 *   input_boolean.kitchen_presentation  presentation mode on/off
 *   input_number.kitchen_presentation_interval  seconds per page
 *   input_number.kitchen_brightness     backlight (the slider; uses the entity's min/max)
 *   input_text.kitchen_dashboard / _view, input_number.kitchen_page  (see below)
 * Until the name is set the board shows a setup screen naming the helper to create. */

/* Dashboard that defines the main-screen pages: each section of the view chosen in the
 * config panel (first page, "View") is one page (tiles = value rows, a line-graph sensor
 * card = graph page). Re-read every interval. */
#define HA_DASHBOARD          "esp32-c6-dev"
#define HA_LAYOUT_INTERVAL_MS 60000

/* Shown dashboard, view and page, kept in sync both ways (the last change wins; flash keeps
 * them when HA is unreachable). Optional: missing or unavailable entities are ignored.
 * input_text: dashboard url_path ("" = HA_DASHBOARD) and view path ("" = the first view);
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
