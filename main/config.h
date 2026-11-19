/* Non-secret device configuration. Wi-Fi credentials and the HA token come from the
 * WIFI_SSID, WIFI_PASS and HA_TOKEN environment variables at build time. */
#pragma once

/* Home Assistant base URL, no trailing slash */
#define HA_URL               "https://home.panzar.io"

/* How often the display reads all data from Home Assistant (entities and the graph
 * history), in milliseconds */
#define HA_POLL_INTERVAL_MS  5000

/* HTTP requests to HA: timeout per attempt, extra attempts after a failure, and the pause
 * before retry n (n * HA_HTTP_RETRY_MS) */
#define HA_HTTP_TIMEOUT_MS   5000
#define HA_HTTP_RETRIES      2
#define HA_HTTP_RETRY_MS     500

/* Toggled by the button; on = dark theme, off = light theme */
#define HA_THEME_ENTITY      "input_boolean.dev_theme"

/* Screen orientation, toggled in Settings; on = landscape, off = portrait */
#define HA_LANDSCAPE_ENTITY  "input_boolean.dev_landscape"

/* Backlight brightness, shown and set by the slider (uses the entity's min/max) */
#define HA_BRIGHTNESS_ENTITY "input_number.dev_brightness"

/* Dashboard that defines the main-screen pages: each section of this view is one page
 * (tiles = value rows, a line-graph sensor card = graph page). Re-read every interval. */
#define HA_DASHBOARD          "esp32-c6-dev"
#define HA_VIEW               "main"
#define HA_LAYOUT_INTERVAL_MS 60000

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
