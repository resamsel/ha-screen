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

/* Main screen pages, switched by dragging left or right. Each page has one row per floor,
 * top to bottom, all sharing the row names below. */
#define HA_SENSOR_PAGES  2      /* temperature, humidity; the graph pages come after */
#define HA_ROW_COUNT     3
#define HA_PAGE_TITLES   { "Temperature", "Humidity", "Packet loss", "Ping" }   /* status line; graph pages last */
#define HA_ROW_NAMES     { "First floor",              "Ground floor",             "Basement" }
#define HA_TEMP_ENTITIES { "sensor.temperature_og",    "sensor.temperature_eg",    "sensor.temperature_ug" }
#define HA_HUM_ENTITIES  { "sensor.luftfeuchtigkeit_og", "sensor.luftfeuchtigkeit_eg", "sensor.luftfeuchtigkeit_ug" }

/* Graph pages, after the sensor pages: last HA_GRAPH_HOURS of each sensor, with its current
 * value on top. One title per graph page goes at the end of HA_PAGE_TITLES. */
#define HA_GRAPH_COUNT       2
#define HA_GRAPH_ENTITIES    { "sensor.www_google_at_paketverlust", "sensor.www_google_at_durchschnittliche_umlaufzeit" }
#define HA_GRAPH_NAMES       { "www.google.at",                     "www.google.at" }
#define HA_GRAPH_DECIMALS    { 1,                                   0 }   /* digits after the point */
#define HA_GRAPH_ZERO_BASED  { 1,                                   0 }   /* 1 = y axis from 0, 0 = fit to the data */
/* Line smoothing: Gaussian blur width in points (pixels); 0 = raw data, higher = smoother.
 * Write decimals with a point (3.0f or 3, not 3f). */
#define HA_GRAPH_SMOOTHING   3.0f
#define HA_GRAPH_LINE_WIDTH  2       /* graph line thickness in pixels */
#define HA_GRAPH_HOURS       24
#define HA_GRAPH_POINTS      168     /* horizontal resolution (1 px per point) */
#define HA_GRAPH_TICK_HOURS  6       /* time labels under the graph, every N hours */

/* A tapped value (with its unit and name) grows by this factor; 1.2 = 20 % bigger */
#define HA_ACTIVE_SCALE      1.6f

/* Local time zone for the graph's time labels (POSIX TZ string; Vienna = CET/CEST) */
#define HA_TIMEZONE          "CET-1CEST,M3.5.0,M10.5.0/3"
