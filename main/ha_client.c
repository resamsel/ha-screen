#include <stdarg.h>
static void add_error(const char *fmt, ...);
/* Wi-Fi station + Home Assistant REST client.
 * All network work runs in its own task so the LVGL UI never blocks. */
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "nvs.h"
#include "ha_client.h"
#include "dash_fetch.h"
#include "cJSON.h"
#include "icon_rule.h"
#include "cache_store.h"
#include "layout_store.h"
#include "names.h"
#include "live.h"
#include "freertos/queue.h"
#include "rom/tjpgd.h"
#include "miniz.h"

#include "config.h"

#include "provision.h"
#include "notify.h"
#include "imu.h"
#include "curve.h"

static const char *TAG = "ha";

#define WIFI_UP_BIT BIT0
#define TOGGLE_BIT  BIT1
#define ORIENT_BIT  BIT7
#define SETBRI_BIT  BIT2
#define WAKE_BIT    BIT3  /* Wi-Fi just came up: check HA right away */
#define REFRESH_BIT BIT4  /* user asked for a refresh */
#define PRESS_BIT   BIT5  /* a dashboard button was tapped */
#define PIC_BIT     BIT6  /* another page is shown: fetch its pictures now */
#define PRES_BIT    BIT8  /* presentation on/off or interval changed */
#define LIVE_BIT    BIT9  /* live state events are queued */
#define NAV_BIT     BIT10 /* dashboard / view / page to write to HA */
#define NIGHT_BIT   BIT11 /* night mode / night brightness / screen timeout changed */
#define TARGET_BIT  BIT12 /* a thermostat's target temperature was changed */
#define MEDIA_BIT   BIT13 /* play/pause or next on a media card */
#define LIGHT_BIT   BIT14 /* a light card was switched or dimmed */
#define TODO_BIT    BIT22 /* a todo-list item was checked or unchecked */
#define PAUSE_BIT   BIT15 /* close all HA connections until RESUME_BIT (OTA update) */
#define PAUSED_BIT  BIT16 /* set by ha_task: paused, no TLS session open */
#define RESUME_BIT  BIT17
#define NOTIFY_BIT  BIT18 /* a notification was dismissed: clear the helper */
#define AUTOROT_BIT BIT19 /* auto-rotate switched on the board */
#define MOTION_BIT  BIT20 /* motion wake switched on the board */
#define SAVER_BIT   BIT21 /* clock screensaver switched on the board */
#define USER_BITS   (AUTOROT_BIT | MOTION_BIT | SAVER_BIT | TOGGLE_BIT | ORIENT_BIT | SETBRI_BIT | PRESS_BIT | TARGET_BIT | MEDIA_BIT | LIGHT_BIT | TODO_BIT | PRES_BIT | NIGHT_BIT | NAV_BIT | NOTIFY_BIT)

static volatile float s_set_value;
static volatile bool s_set_dark, s_set_landscape;   /* values for TOGGLE_BIT / ORIENT_BIT */

static EventGroupHandle_t s_events;
static ha_state_cb_t s_state_cb;
static ha_landscape_cb_t s_land_cb;
void ha_client_set_landscape_cb(ha_landscape_cb_t cb) { s_land_cb = cb; }
static ha_pres_cb_t s_pres_cb;
void ha_client_set_pres_cb(ha_pres_cb_t cb) { s_pres_cb = cb; }
static ha_night_cb_t s_night_cb;
void ha_client_set_night_cb(ha_night_cb_t cb) { s_night_cb = cb; }
static ha_autorot_cb_t s_autorot_cb;
void ha_client_set_autorot_cb(ha_autorot_cb_t cb) { s_autorot_cb = cb; }
static volatile bool s_set_autorot;
static ha_autorot_cb_t s_motion_cb;
void ha_client_set_motion_cb(ha_autorot_cb_t cb) { s_motion_cb = cb; }
static volatile bool s_set_motion;
static ha_autorot_cb_t s_saver_cb;
void ha_client_set_saver_cb(ha_autorot_cb_t cb) { s_saver_cb = cb; }
static volatile bool s_set_saver;
static volatile int s_set_night, s_set_night_pct, s_set_scr_to;
static volatile bool s_set_pres_on;
static volatile int s_set_pres_secs;
static ha_brightness_cb_t s_brightness_cb;
static ha_done_cb_t s_done_cb;
static ha_sensor_cb_t s_sensor_cb;
static ha_graph_cb_t s_graph_cb;
static bool s_force_report;
static ha_layout_cb_t s_layout_cb;
/* A dashboard / view switch (issue #83): the new layout is held back from the UI, which keeps
 * the old view, until its states are read; card data meanwhile is only recorded. */
static volatile bool s_hold_next;         /* the next refresh round is a switch */
static bool s_hold;                       /* this round: don't show card data */
static bool s_held_layout;                /* use_layout ran while holding: send it on release */
static bool s_held_theme;                 /* its theme, read on release */
static void fetch_theme(const char *name);
void ha_client_set_layout_cb(ha_layout_cb_t cb) { s_layout_cb = cb; }
static ha_theme_cb_t s_theme_cb;
void ha_client_set_theme_cb(ha_theme_cb_t cb) { s_theme_cb = cb; }
static ha_busy_cb_t s_busy_cb;
static ha_button_cb_t s_button_cb;
static ha_press_cb_t s_press_cb;
void ha_client_set_button_cb(ha_button_cb_t cb) { s_button_cb = cb; }
void ha_client_set_press_cb(ha_press_cb_t cb) { s_press_cb = cb; }
static volatile int s_press_page = -1, s_press_row = -1;   /* last tapped button */
void ha_client_set_busy_cb(ha_busy_cb_t cb) { s_busy_cb = cb; }
static ha_layout_t s_layout;              /* current layout, used only by the network task */
static ha_picture_cb_t s_pic_cb;
void ha_client_set_picture_cb(ha_picture_cb_t cb) { s_pic_cb = cb; }
static ha_icon_cb_t s_icon_cb;
void ha_client_set_icon_cb(ha_icon_cb_t cb) { s_icon_cb = cb; }
static char *s_icon_rule[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];      /* from HA's icon tables */
static char s_icon_last[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS][48];   /* last icon reported */
static ha_climate_cb_t s_climate_cb;
void ha_client_set_climate_cb(ha_climate_cb_t cb) { s_climate_cb = cb; }
static volatile int s_tgt_page = -1, s_tgt_row = -1;
static volatile float s_tgt_value;
static ha_media_cb_t s_media_cb;
void ha_client_set_media_cb(ha_media_cb_t cb) { s_media_cb = cb; }
static volatile int s_med_page = -1, s_med_row = -1, s_med_cmd;
static ha_light_cb_t s_light_cb;
void ha_client_set_light_cb(ha_light_cb_t cb) { s_light_cb = cb; }
static ha_todo_cb_t s_todo_cb;
void ha_client_set_todo_cb(ha_todo_cb_t cb) { s_todo_cb = cb; }
static portMUX_TYPE s_todo_lock = portMUX_INITIALIZER_UNLOCKED;
typedef struct { int8_t page, row; bool done; char uid[40]; } todo_req_t;
static todo_req_t s_todo_q[8];     /* taps not yet sent: quick ones must not overwrite each other */
static int s_todo_n;
static volatile int s_lt_page = -1, s_lt_row = -1, s_lt_pct;
static ha_weather_cb_t s_weather_cb;
void ha_client_set_weather_cb(ha_weather_cb_t cb) { s_weather_cb = cb; }
static ha_forecast_cb_t s_forecast_cb;
void ha_client_set_forecast_cb(ha_forecast_cb_t cb) { s_forecast_cb = cb; }
static ha_calendar_cb_t s_calendar_cb;
void ha_client_set_calendar_cb(ha_calendar_cb_t cb) { s_calendar_cb = cb; }
static portMUX_TYPE s_pic_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_pic_page = -1;               /* page on screen, and its picture block sizes */
static uint16_t s_pic_w[LAYOUT_MAX_ROWS], s_pic_h[LAYOUT_MAX_ROWS];
static uint32_t s_layout_hash;
static ha_views_cb_t s_views_cb;
void ha_client_set_views_cb(ha_views_cb_t cb) { s_views_cb = cb; }
static portMUX_TYPE s_view_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_view[32];                   /* view path chosen in the config panel, "" = first */
static char s_dash[32];                   /* dashboard url_path chosen there, "" = none chosen yet */
static ha_dashboards_cb_t s_dashes_cb;
void ha_client_set_dashboards_cb(ha_dashboards_cb_t cb) { s_dashes_cb = cb; }

/* Dashboard / view / page entities: the last values read (valid flags), and the values to write */
static ha_nav_cb_t s_nav_cb;
void ha_client_set_nav_cb(ha_nav_cb_t cb) { s_nav_cb = cb; }
static ha_notify_cb_t s_notify_cb;
void ha_client_set_notify_cb(ha_notify_cb_t cb) { s_notify_cb = cb; }
static char s_nav_dash[32], s_nav_view[32];
static bool s_nav_dash_ok, s_nav_view_ok;
static int s_nav_page = -1;
static char s_wnav_dash[32], s_wnav_view[32];   /* under s_view_lock */
static int s_wnav_page = -1;
static volatile bool s_nav_writing;           /* a write is queued: HA's old values are ignored */
static volatile TickType_t s_nav_wrote = (TickType_t)0 - pdMS_TO_TICKS(HA_ECHO_HOLD_MS);      /* when the last write finished: echoes until HA_ECHO_HOLD_MS */

static bool json_str(const char *json, const char *key, char *out, size_t n);
static bool json_num(const char *json, const char *key, float *out);

/* input_text state; false when unknown / unavailable */
static bool nav_text(const char *json, char *out, size_t n)
{
    char st[32];
    if (!json_str(json, "\"state\":\"", st, sizeof(st)) || !strcmp(st, "unknown") || !strcmp(st, "unavailable"))
        return false;
    snprintf(out, n, "%s", st);
    return true;
}

/* input_number state, counted from 1 -> 0-based page; -1 when not a number */
static int nav_page(const char *json)
{
    float v;
    if (!json_num(json, "\"state\":\"", &v) || v < 1 || v > 99) return -1;
    return (int)(v + 0.5f) - 1;
}

static void nav_report(void)
{
    if (s_nav_cb && !s_nav_writing && xTaskGetTickCount() - s_nav_wrote >= pdMS_TO_TICKS(HA_ECHO_HOLD_MS))
        s_nav_cb(s_nav_dash_ok ? s_nav_dash : NULL, s_nav_view_ok ? s_nav_view : NULL, s_nav_page);
}

/* lovelace/dashboards/list result -> dashboards, reported to the UI */
static void report_dashboards(const char *msg, const char *cur)
{
    static layout_view_t d[LAYOUT_MAX_VIEWS];
    int current, n = layout_parse_dashboards(msg, cur, d, LAYOUT_MAX_VIEWS, &current);
    if (!n) return;                       /* failed or empty: keep what the UI has */
    if (s_dashes_cb) s_dashes_cb(d, n, current);
}

/* Wi-Fi or HA never worked since boot (wrong password, URL or token; moved board): reboot
 * into the setup portal with the reason. The portal returns to normal after a while if
 * nobody uses it (router or HA outage). Stopped by HA's first answer that accepts the token. */
static esp_timer_handle_t s_nowifi_timer;
static volatile bool s_wifi_ever;           /* got an IP since boot */
static volatile int s_ha_last = -1;         /* last HA status: -1 no answer, 401/403 token */
static void nowifi_fired(void *arg)
{
    char why[160];
    if (!s_wifi_ever)
        snprintf(why, sizeof(why), "Couldn't join the Wi-Fi \"%s\". Check its name and password.", prov_ssid());
    else if (s_ha_last == 401 || s_ha_last == 403)
        snprintf(why, sizeof(why), "Home Assistant rejected the access token.");
    else
        snprintf(why, sizeof(why), "Couldn't reach Home Assistant at %s. Check the URL.", prov_url());
    ESP_LOGW(TAG, "%s Opening the setup portal.", why);
    prov_request_portal(why);
    esp_restart();
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, WIFI_UP_BIT);
        if (s_state_cb) s_state_cb(HA_STATE_OFFLINE);
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Wi-Fi up, IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_wifi_ever = true;
        xEventGroupSetBits(s_events, WIFI_UP_BIT | WAKE_BIT);
    }
}

/* Small response buffer: we only need the "state" field */
typedef struct { char buf[2048]; int len; } resp_t;

/* One HTTPS connection is kept open and reused for every request, so the TLS
 * handshake (the slow part) happens only once instead of per request. */
static esp_http_client_handle_t s_http;
static resp_t *s_resp;
static resp_t s_rbuf;   /* the one response buffer (2 KB) for the HA task's requests */
/* Forecast responses are parsed while they stream in (they can be large): each entry
 * object after "forecast":[ is collected and handed to fc_entry */
static bool s_fc_mode;
static char s_fc_obj[320];
static int s_fc_len, s_fc_state;          /* 0 = before the list, 1 = in it, 2 = in an entry, 3 = done */
static void fc_entry(char *obj);
static ha_forecast_t s_fc[HA_FORECAST_MAX];
static int s_fc_n;
static cal_parse_t *s_cal;                /* calendar.get_events response being parsed, or NULL */

/* ---- 24 h history, parsed while it streams in (the response can be large) ---- */
static bool s_hist_mode;                  /* route response body to the history parser */
static int64_t s_now;                     /* server time from the HTTP Date header (UTC s) */
static float s_hist_bucket[HA_GRAPH_POINTS];
static float s_hist_last[HA_GRAPH_POINTS];    /* value in effect at the end of the bucket */
static bool s_hist_has[HA_GRAPH_POINTS];
static char s_hist_line[384];
static int s_hist_len;
static int s_hist_depth;                  /* {} nesting: entries are the depth-1 objects */
static int s_hist_hours = LAYOUT_HOURS_DEFAULT;  /* span of the graph being fetched */

/* days since 1970-01-01 for a civil date (Howard Hinnant's algorithm) */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* "2026-11-19T13:05:12.123+00:00" -> UTC seconds (HA sends UTC) */
static int64_t parse_iso(const char *t)
{
    int Y, M, D, h, mi, sec;
    if (sscanf(t, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &mi, &sec) != 6) return 0;
    return days_from_civil(Y, M, D) * 86400 + h * 3600 + mi * 60 + sec;
}

/* "Thu, 19 Nov 2026 13:05:12 GMT" -> UTC seconds */
static int64_t parse_http_date(const char *t)
{
    static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char ms[4] = "";
    int D, Y, h, mi, sec;
    if (sscanf(t, "%*3s, %d %3s %d %d:%d:%d", &D, ms, &Y, &h, &mi, &sec) != 6) return 0;
    const char *f = strstr(mon, ms);
    if (!f) return 0;
    return days_from_civil(Y, (int)(f - mon) / 3 + 1, D) * 86400 + h * 3600 + mi * 60 + sec;
}

/* Value v at time ts (UTC seconds) into its bucket of the graph being fetched */
static void hist_add(int64_t ts, float v)
{
    int64_t start = s_now - (int64_t)s_hist_hours * 3600;
    int b = ts <= start ? 0 : (int)((ts - start) * HA_GRAPH_POINTS / ((int64_t)s_hist_hours * 3600));
    if (b < 0 || b >= HA_GRAPH_POINTS) return;
    if (!s_hist_has[b] || v > s_hist_bucket[b]) s_hist_bucket[b] = v;  /* keep spikes */
    s_hist_last[b] = v;                   /* history is in time order: last one wins */
    s_hist_has[b] = true;
}

/* One history entry: {"state":"0.0","last_changed":"..."} */
static void hist_point(const char *obj)
{
    const char *st = strstr(obj, "\"state\":\"");
    const char *lc = strstr(obj, "\"last_changed\":\"");
    if (!st || !lc || !s_now) return;
    char *end;
    float v = strtof(st + 9, &end);
    if (end == st + 9) return;            /* "unavailable" etc. */
    hist_add(parse_iso(lc + 16), v);
}

static void hist_feed(const char *data, int n)
{
    for (int i = 0; i < n; i++) {
        char c = data[i];
        /* an entry can hold nested objects ("attributes":{}), so only the outer
         * braces start and end an entry */
        if (c == '{' && ++s_hist_depth == 1) {
            s_hist_len = 0;
        } else if (c == '}' && s_hist_depth > 0 && --s_hist_depth == 0) {
            s_hist_line[s_hist_len] = 0;
            hist_point(s_hist_line);
            s_hist_len = 0;
        } else if (s_hist_depth > 0 && s_hist_len < (int)sizeof(s_hist_line) - 1) {
            s_hist_line[s_hist_len++] = c;
        }
    }
}

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    resp_t *r = s_resp;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(evt->header_key, "Date") == 0) {
        int64_t t = parse_http_date(evt->header_value);
        if (t) {
            s_now = t;
            /* HA's clock is the board's clock (for the clock page); adjust when off by > 2 s */
            time_t cur = time(NULL);
            if (cur < t - 2 || cur > t + 2) {
                struct timeval tv = { .tv_sec = (time_t)t };
                settimeofday(&tv, NULL);
            }
        }
    }
    if (evt->event_id == HTTP_EVENT_ON_DATA && s_hist_mode) {
        hist_feed(evt->data, evt->data_len);
        return ESP_OK;
    }
    if (evt->event_id == HTTP_EVENT_ON_DATA && r) {
        int n = evt->data_len;
        if (r->len + n >= (int)sizeof(r->buf)) n = sizeof(r->buf) - 1 - r->len;
        memcpy(r->buf + r->len, evt->data, n);
        r->len += n;
        r->buf[r->len] = 0;
    }
    if (evt->event_id == HTTP_EVENT_ON_DATA && s_cal) {
        cal_parse_feed(s_cal, evt->data, evt->data_len);
        return ESP_OK;
    }
    if (evt->event_id == HTTP_EVENT_ON_DATA && s_fc_mode) {
        static const char key[] = "\"forecast\":[";
        const char *d = evt->data;
        for (int k = 0; k < evt->data_len; k++) {
            char c = d[k];
            if (s_fc_state == 0) {        /* match the key across chunk borders */
                s_fc_len = c == key[s_fc_len] ? s_fc_len + 1 : (c == key[0]);
                if (s_fc_len == (int)sizeof(key) - 1) { s_fc_state = 1; s_fc_len = 0; }
            } else if (s_fc_state == 1) {
                if (c == '{') { s_fc_state = 2; s_fc_len = 0; s_fc_obj[s_fc_len++] = c; }
                else if (c == ']') s_fc_state = 3;
            } else if (s_fc_state == 2) {
                if (s_fc_len < (int)sizeof(s_fc_obj) - 1) s_fc_obj[s_fc_len++] = c;
                if (c == '}') { s_fc_obj[s_fc_len] = 0; s_fc_state = 1; fc_entry(s_fc_obj); }
            }
        }
    }
    return ESP_OK;
}

/* One attempt; returns the HTTP status, or -1 with *err set on a transport error */
static int ha_request_once(esp_http_client_method_t method, const char *url, const char *body,
                           resp_t *resp, esp_err_t *err)
{
    if (!s_http) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = HA_HTTP_TIMEOUT_MS,
            .keep_alive_enable = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler = http_event,
        };
        s_http = esp_http_client_init(&cfg);
        esp_http_client_set_header(s_http, "Authorization", prov_bearer());
        esp_http_client_set_header(s_http, "Content-Type", "application/json");
    }
    if (resp) { resp->len = 0; resp->buf[0] = 0; }
    if (s_fc_mode) { s_fc_state = 0; s_fc_len = 0; s_fc_n = 0; }   /* fresh on retry */
    if (s_hist_mode) { memset(s_hist_has, 0, sizeof(s_hist_has)); s_hist_len = 0; s_hist_depth = 0; } /* fresh on retry */
    if (s_cal) cal_parse_init(s_cal);   /* fresh on retry */
    s_resp = resp;
    esp_http_client_set_url(s_http, url);
    esp_http_client_set_method(s_http, method);
    esp_http_client_set_post_field(s_http, body, body ? strlen(body) : 0);
    *err = esp_http_client_perform(s_http);
    s_resp = NULL;
    /* a read that failed midway (e.g. TLS out of memory) can still return ESP_OK; that
     * connection is broken and reusing it crashes mbedTLS, so treat it as a transport error */
    if (*err == ESP_OK && !esp_http_client_is_complete_data_received(s_http)) *err = ESP_FAIL;
    if (*err != ESP_OK) {
        esp_http_client_cleanup(s_http); /* drop the (possibly stale) connection */
        s_http = NULL;
        return -1;
    }
    return esp_http_client_get_status_code(s_http);
}

/* Request with retries: transport errors, 5xx and 429 are retried up to HA_HTTP_RETRIES
 * times with growing pauses; 4xx are final. Toggles aren't idempotent, so they are only
 * retried when the connection couldn't be opened (the request never reached HA). */
static bool s_optional;                /* the request is for an optional helper: 404 is fine */
static int ha_request(esp_http_client_method_t method, const char *path, const char *body, resp_t *resp)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", prov_url(), path);
    bool idempotent = strstr(path, "/toggle") == NULL;
    esp_err_t err = ESP_OK;
    int status = -1;
    for (int attempt = 0; attempt <= HA_HTTP_RETRIES; attempt++) {
        if (attempt) {
            ESP_LOGW(TAG, "%s: retry %d", path, attempt);
            vTaskDelay(pdMS_TO_TICKS(HA_HTTP_RETRY_MS * attempt));
        }
        status = ha_request_once(method, url, body, resp, &err);
        bool retry = status < 0 ? (idempotent || err == ESP_ERR_HTTP_CONNECT)
                                : idempotent && (status >= 500 || status == 429);
        if (!retry) break;
    }
    s_ha_last = status;
    if (status >= 0 && status != 401 && status != 403 && s_nowifi_timer) {
        esp_timer_stop(s_nowifi_timer);     /* HA answered and took the token: setup works */
        esp_timer_delete(s_nowifi_timer);
        s_nowifi_timer = NULL;
    }
    if (status < 0) {
        ESP_LOGE(TAG, "%s failed: %s", path, esp_err_to_name(err));
        add_error("%s: %s", path, esp_err_to_name(err));
    } else if (status == 404 && s_optional) {
        /* an optional helper that doesn't exist: not an error */
    } else if (status >= 300) {
        ESP_LOGE(TAG, "%s -> HTTP %d", path, status);
        add_error("%s: HTTP %d", path, status);
    }
    return status;
}

static char s_err[640];
static ha_error_cb_t s_err_cb;
void ha_client_set_error_cb(ha_error_cb_t cb) { s_err_cb = cb; }

static void add_error(const char *fmt, ...)
{
    size_t n = strlen(s_err);
    if (n >= sizeof(s_err) - 2) return;
    if (n) s_err[n++] = '\n';
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_err + n, sizeof(s_err) - n, fmt, ap);
    va_end(ap);
}

/* ---- Board name (issue #24) --------------------------------------------------------
 * Each board has an ID from its MAC (screen_a1b2c3). The HA helper input_text.<id> holds
 * the board's name; the name prefixes the board's helpers (kitchen -> input_text.
 * kitchen_dashboard, ...). Until it's set the setup screen shows the helper to create.
 * The last name is kept in NVS so a slow HA at boot doesn't bring the setup screen back. */
static char s_board_id[20];             /* screen_a1b2c3 */
static char s_name_ent[48];             /* input_text.screen_a1b2c3 */
static char s_board[32];                /* the name ("" = not set) */
static char s_board_raw[32];            /* the name as typed in the setup portal */
static bool s_name_push;                /* portal saved a name: write it to the HA helper */
/* Bump when a firmware adds board helpers: boards set up before then create the new ones
 * once (2: night, night brightness, screen timeout, issue #17; 3: notification, issue #19; 4: auto-rotate, issue #42) */
#define HELPERS_VERSION 7
static bool s_mkhelp;                   /* portal saved: create the missing helpers once */
static ha_local_cb_t s_local_cb;
void ha_client_set_local_cb(ha_local_cb_t cb) { s_local_cb = cb; }
static volatile bool s_name_changed;    /* live event: the name entity changed */
static struct {
    char theme[80], land[80], pres[80], pres_int[80], bri[80], dash[80], view[80], page[80];
    char night[80], night_bri[80], scr_to[80], notify[80], autorot[80], motion[80], saver[80];
} s_ent;
/* Which board helpers HA has: 1 yes, 0 no (404), -1 not asked yet. Every helper is optional:
 * without one the board keeps that setting itself, and writes to a missing one are skipped. */
enum { H_THEME, H_LAND, H_PRES, H_PRES_INT, H_BRI, H_DASH, H_VIEW, H_PAGE, H_NIGHT, H_NIGHT_BRI, H_SCR_TO, H_NOTIFY, H_AUTOROT, H_MOTION, H_SAVER, H_COUNT };
static int8_t s_has[H_COUNT] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static bool has(int h) { return s_has[h] != 0; }
static ha_setup_cb_t s_setup_cb;
void ha_client_set_setup_cb(ha_setup_cb_t cb) { s_setup_cb = cb; }

static int s_ln_night = -1, s_ln_pct = -1, s_ln_to = -1;   /* night helpers, as last received */

static void ent_build(void)
{
    const char *b = s_board;
    snprintf(s_ent.theme, 80, "input_boolean.%s_theme", b);
    snprintf(s_ent.land, 80, "input_boolean.%s_landscape", b);
    snprintf(s_ent.pres, 80, "input_boolean.%s_presentation", b);
    snprintf(s_ent.pres_int, 80, "input_number.%s_presentation_interval", b);
    snprintf(s_ent.bri, 80, "input_number.%s_brightness", b);
    snprintf(s_ent.dash, 80, "input_text.%s_dashboard", b);
    snprintf(s_ent.view, 80, "input_text.%s_view", b);
    snprintf(s_ent.page, 80, "input_number.%s_page", b);
    snprintf(s_ent.night, 80, "input_boolean.%s_night", b);
    snprintf(s_ent.night_bri, 80, "input_number.%s_night_brightness", b);
    snprintf(s_ent.scr_to, 80, "input_number.%s_screen_timeout", b);   /* seconds */
    snprintf(s_ent.notify, 80, "input_text.%s_notification", b);
    snprintf(s_ent.autorot, 80, "input_boolean.%s_auto_rotate", b);
    snprintf(s_ent.motion, 80, "input_boolean.%s_motion_wake", b);
    snprintf(s_ent.saver, 80, "input_boolean.%s_screensaver", b);
}

/* Like HA's object IDs: lowercase, runs of anything else become one '_', no '_' at the ends */
static void board_slug(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in && o + 1 < n; in++) {
        char c = *in;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[o++] = c;
        else if (o && out[o - 1] != '_') out[o++] = '_';
    }
    while (o && out[o - 1] == '_') o--;
    out[o] = 0;
}

/* The board works without the name helper (the name comes from the setup portal) */
static void board_report(void)
{
    if (s_setup_cb) s_setup_cb(NULL, s_board);
}

/* New name (already a slug, "" = not set): saved to NVS, helper IDs rebuilt. True if changed. */
static bool board_set(const char *name)
{
    if (!strcmp(name, s_board)) return false;
    ESP_LOGI(TAG, "board name: '%s' -> '%s'", s_board, name);
    snprintf(s_board, sizeof(s_board), "%s", name);
    ent_build();
    for (int h = 0; h < H_COUNT; h++) s_has[h] = -1;   /* new prefix: ask HA again */
    s_mkhelp = true;                      /* and create the new name's missing helpers */
    nvs_handle_t h;
    if (nvs_open("board", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "name", s_board);
        nvs_commit(h);
        nvs_close(h);
    }
    board_report();
    return true;
}

const char *ha_client_board_id(void) { return s_board_id; }

static void board_init(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(s_board_id, sizeof(s_board_id), "screen_%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_name_ent, sizeof(s_name_ent), "input_text.%s", s_board_id);
    nvs_handle_t h;
    size_t n = sizeof(s_board);
    if (nvs_open("board", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "name", s_board, &n) != ESP_OK) s_board[0] = 0;
        nvs_close(h);
    }
    uint8_t push = 0;
    if (nvs_open("board", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "push", &push);
        nvs_close(h);
    }
    s_name_push = push;
    uint8_t mk = 0;
    if (nvs_open("board", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "mkhelp", &mk);
        uint8_t ver = 0;
        nvs_get_u8(h, "helpver", &ver);
        if (ver < HELPERS_VERSION) mk = 1;   /* firmware added helpers: create the missing ones once */
        nvs_close(h);
    }
    s_mkhelp = mk;
    snprintf(s_board_raw, sizeof(s_board_raw), "%s", s_board);   /* the portal stores it as typed */
    board_slug(s_board_raw, s_board, sizeof(s_board));
    if (!s_board[0]) snprintf(s_board, sizeof(s_board), "%s", s_board_id);   /* never named */
    ent_build();
    ESP_LOGI(TAG, "board %s, name '%s'", s_board_id, s_board);
}

/* The name helper's state: a name, "" when set to nothing / unknown / unavailable */
static void board_parse(const char *json, char *out, size_t n);

/* Read the name from HA. 200 or 404 is an answer (404 = helper not created: no name);
 * anything else keeps the cached name. True if the name changed. */
static bool fetch_name(void);

/* ---- Device diagnostics (issue #16): posted to HA as sensor.<board name>_* ---- */
static const char *diag_name(void) { return s_board[0] ? s_board : s_board_id; }
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power_on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:  return "task_watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    case ESP_RST_EFUSE:     return "efuse";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default:                return "unknown";
    }
}

/* Copy src into dst as the inside of a JSON string (quotes, backslashes, control chars) */
static void json_escape(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    for (; *src && o + 7 < n; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = c; }
        else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
        else if (c < 0x20) o += snprintf(dst + o, n - o, "\\u%04x", c);
        else dst[o++] = c;
    }
    dst[o] = 0;
}

static void diag_post(const char *id, const char *body)
{
    char path[96];
    snprintf(path, sizeof(path), "/api/states/sensor.%s_%s", diag_name(), id);
    resp_t *r = &s_rbuf;   /* shared: all callers run on the HA task, one at a time */
    r->len = 0;
    ha_request(HTTP_METHOD_POST, path, body, r);
}

/* A round's errors go to HA's logbook (logbook.log), one entry each, attached to
 * sensor.<board name>_firmware so a Logbook card on that entity shows them; prefixed with
 * device name, id (MAC) and firmware. A round with the same errors as the last logged one
 * is skipped so a lasting problem isn't logged every poll. */
static void report_errors(void)
{
    static char last[sizeof(s_err)], cur[sizeof(s_err)];
    static char msg[2 * sizeof(s_err)], body[sizeof(msg) + 96];
    resp_t *r = &s_rbuf;   /* shared: all callers run on the HA task, one at a time */
    static char meta[128];
    if (!s_err[0] || !strcmp(s_err, last)) return;
    {                              /* device=<name> id=<base MAC> fw=<version> */
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_BASE);
        snprintf(meta, sizeof(meta), "[device=%s id=%02x%02x%02x%02x%02x%02x fw=%.31s] ", diag_name(),
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], esp_app_get_description()->version);
    }
    strcpy(cur, s_err);            /* posting may append to s_err */
    strcpy(last, cur);
    for (char *line = strtok(cur, "\n"); line; line = strtok(NULL, "\n")) {
        char full[sizeof(meta) + sizeof(s_err)];
        snprintf(full, sizeof(full), "%s%s", meta, line);
        json_escape(msg, sizeof(msg), full);
        snprintf(body, sizeof(body), "{\"name\":\"%s error\",\"message\":\"%s\","
                 "\"entity_id\":\"sensor.%s_firmware\",\"domain\":\"sensor\"}", diag_name(), msg, diag_name());
        r->len = 0;
        ha_request(HTTP_METHOD_POST, "/api/services/logbook/log", body, r);
    }
}

/* ---- IMU (issue #39): main.c hands over each reading, the HA task posts them ---- */
static portMUX_TYPE s_imu_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_imu[6];
static bool s_imu_have;
static const char *s_imu_posted;   /* orientation last posted, NULL = none yet */

void ha_client_set_imu(int ax, int ay, int az, int gx, int gy, int gz)
{
    portENTER_CRITICAL(&s_imu_lock);
    s_imu[0] = ax; s_imu[1] = ay; s_imu[2] = az; s_imu[3] = gx; s_imu[4] = gy; s_imu[5] = gz;
    s_imu_have = true;
    portEXIT_CRITICAL(&s_imu_lock);
}

static bool imu_get(int v[6])
{
    portENTER_CRITICAL(&s_imu_lock);
    bool have = s_imu_have;
    memcpy(v, s_imu, sizeof(s_imu));
    portEXIT_CRITICAL(&s_imu_lock);
    return have;
}

/* Orientation, posted every round it differs from the last one posted (or with all = always) */
static void report_orientation(bool all)
{
    int v[6];
    if (!imu_get(v)) return;
    const char *o = imu_orientation(v[0], v[1], v[2]);
    if (!all && o == s_imu_posted) return;
    char body[256];
    snprintf(body, sizeof(body), "{\"state\":\"%s\",\"attributes\":{\"friendly_name\":\"%s orientation\","
             "\"device_class\":\"enum\",\"options\":[\"portrait\",\"portrait_flipped\",\"landscape\","
             "\"landscape_flipped\",\"flat\",\"face_down\",\"tilted\"],\"icon\":\"mdi:phone-rotate-portrait\"}}",
             o, diag_name());
    diag_post("orientation", body);
    s_imu_posted = o;
}

static void report_imu(void)
{
    int v[6];
    if (!imu_get(v)) return;
    char body[384];
    float ax = IMU_ACC_G(v[0]), ay = IMU_ACC_G(v[1]), az = IMU_ACC_G(v[2]);
    snprintf(body, sizeof(body), "{\"state\":%.2f,\"attributes\":{\"friendly_name\":\"%s acceleration\","
             "\"unit_of_measurement\":\"g\",\"state_class\":\"measurement\",\"icon\":\"mdi:axis-arrow\","
             "\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}}",
             sqrtf(ax * ax + ay * ay + az * az), diag_name(), ax, ay, az);
    diag_post("acceleration", body);
    float gx = IMU_GYR_DPS(v[3]), gy = IMU_GYR_DPS(v[4]), gz = IMU_GYR_DPS(v[5]);
    snprintf(body, sizeof(body), "{\"state\":%.1f,\"attributes\":{\"friendly_name\":\"%s gyroscope\","
             "\"unit_of_measurement\":\"°/s\",\"state_class\":\"measurement\",\"icon\":\"mdi:rotate-3d-variant\","
             "\"x\":%.1f,\"y\":%.1f,\"z\":%.1f}}",
             sqrtf(gx * gx + gy * gy + gz * gz), diag_name(), gx, gy, gz);
    diag_post("gyroscope", body);
    snprintf(body, sizeof(body), "{\"state\":%.0f,\"attributes\":{\"friendly_name\":\"%s tilt\","
             "\"unit_of_measurement\":\"°\",\"state_class\":\"measurement\",\"icon\":\"mdi:angle-acute\"}}",
             imu_tilt(v[0], v[1], v[2]), diag_name());
    diag_post("tilt", body);
    report_orientation(true);
}

static void report_diag(void)
{
    char body[512];
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(body, sizeof(body), "{\"state\":%d,\"attributes\":{\"friendly_name\":\"%s Wi-Fi signal\","
                 "\"unit_of_measurement\":\"dBm\",\"device_class\":\"signal_strength\",\"state_class\":\"measurement\","
                 "\"ssid\":\"%.32s\",\"channel\":%d}}", ap.rssi, diag_name(), (const char *)ap.ssid, ap.primary);
        diag_post("wifi_rssi", body);
    }
    snprintf(body, sizeof(body), "{\"state\":%u,\"attributes\":{\"friendly_name\":\"%s free heap\","
             "\"unit_of_measurement\":\"B\",\"device_class\":\"data_size\",\"state_class\":\"measurement\","
             "\"minimum_free\":%u,\"largest_free_block\":%u}}",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), diag_name(),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    diag_post("free_heap", body);
    snprintf(body, sizeof(body), "{\"state\":%u,\"attributes\":{\"friendly_name\":\"%s largest free block\","
             "\"unit_of_measurement\":\"B\",\"device_class\":\"data_size\",\"state_class\":\"measurement\"}}",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), diag_name());
    diag_post("largest_free_block", body);
    snprintf(body, sizeof(body), "{\"state\":%lld,\"attributes\":{\"friendly_name\":\"%s uptime\","
             "\"unit_of_measurement\":\"s\",\"device_class\":\"duration\",\"state_class\":\"total_increasing\"}}",
             (long long)(esp_timer_get_time() / 1000000), diag_name());
    diag_post("uptime", body);
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(body, sizeof(body), "{\"state\":\"%.31s\",\"attributes\":{\"friendly_name\":\"%s firmware\","
             "\"built\":\"%.15s %.15s\",\"idf\":\"%.31s\",\"reset_reason\":\"%s\"}}",
             app->version, diag_name(), app->date, app->time, app->idf_ver, reset_reason_str(esp_reset_reason()));
    diag_post("firmware", body);
    report_imu();
}

/* Find key in JSON text and parse the number right after it */
static bool json_num(const char *json, const char *key, float *out)
{
    const char *p = strstr(json, key);
    if (!p) return false;
    p += strlen(key);
    char *end;
    float v = strtof(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

/* ---- Card data cache: the last data sent for every card, saved to flash (cache_store) and
 * replayed through the same callbacks at boot, so the board shows it before HA answers ---- */
enum { REC_VALUE, REC_GLATEST, REC_GPOINTS, REC_BUTTON, REC_WEATHER, REC_ICON, REC_FORECAST, REC_CALENDAR, REC_CLIMATE, REC_MEDIA, REC_LIGHT, REC_TYPES };
static uint8_t *s_rec[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS][REC_TYPES];
static uint16_t s_rec_len[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS][REC_TYPES];
static uint8_t s_rec_theme = 0xFF;        /* HA_STATE_ON/OFF of the theme entity */
static float s_rec_bri[3];
static bool s_rec_bri_has, s_rec_dirty;

/* Guards the records and s_layout against ha_client_replay from the UI task. Recursive:
 * a replay re-enters rec_put through the emit_* functions. Never held while waiting for the
 * LVGL lock from the network task (callbacks run after rec_put has released it). */
static SemaphoreHandle_t s_rec_mx;
static void rec_lock(void) { if (s_rec_mx) xSemaphoreTakeRecursive(s_rec_mx, portMAX_DELAY); }
static void rec_unlock(void) { if (s_rec_mx) xSemaphoreGiveRecursive(s_rec_mx); }

static void rec_put(int type, int pg, int row, const void *d, size_t n)
{
    if (pg < 0 || pg >= LAYOUT_MAX_PAGES || row < 0 || row >= LAYOUT_MAX_ROWS || n > 4096) return;
    rec_lock();
    if (!(s_rec[pg][row][type] && s_rec_len[pg][row][type] == n && !memcmp(s_rec[pg][row][type], d, n))) {
        uint8_t *b = realloc(s_rec[pg][row][type], n ? n : 1);
        if (b) {
            memmove(b, d, n);
            s_rec[pg][row][type] = b;
            s_rec_len[pg][row][type] = n;
            s_rec_dirty = true;
        }
    }
    rec_unlock();
}

static void rec_clear(void)
{
    rec_lock();
    for (int p = 0; p < LAYOUT_MAX_PAGES; p++)
        for (int i = 0; i < LAYOUT_MAX_ROWS; i++)
            for (int t = 0; t < REC_TYPES; t++) { free(s_rec[p][i][t]); s_rec[p][i][t] = NULL; s_rec_len[p][i][t] = 0; }
    rec_unlock();
}

/* strings back to back, each with its NUL */
static size_t pack_strs(uint8_t *out, size_t cap, const char *const *a, int k)
{
    size_t n = 0;
    for (int i = 0; i < k; i++) {
        size_t l = strlen(a[i] ? a[i] : "") + 1;
        if (n + l > cap) break;
        memcpy(out + n, a[i] ? a[i] : "", l);
        n += l;
    }
    return n;
}

static const char *unpack_str(const uint8_t **p, const uint8_t *end)
{
    const char *s = (const char *)*p;
    const uint8_t *z = memchr(*p, 0, end - *p);
    if (!z) return "";
    *p = z + 1;
    return s;
}

static void emit_sensor(int pg, int row, const char *text, const char *unit, const char *name)
{
    uint8_t b[96];
    const char *a[] = { text, unit, name };
    rec_put(REC_VALUE, pg, row, b, pack_strs(b, sizeof(b), a, 3));
    if (s_hold) return;
    if (s_sensor_cb) s_sensor_cb(pg, row, text, unit, name);
}

static void emit_graph(int pg, int row, const char *latest, const char *unit, const char *name,
                       const float *pts, int n, int64_t end, int hours, bool zero_based)
{
    if (latest) {
        uint8_t b[96];
        const char *a[] = { latest, unit, name };
        rec_put(REC_GLATEST, pg, row, b, pack_strs(b, sizeof(b), a, 3));
    }
    if (pts && n > 0 && n <= HA_GRAPH_POINTS) {
        static uint8_t b[16 + HA_GRAPH_POINTS * sizeof(float)];
        uint16_t hh = hours, nn = n;
        b[0] = zero_based;
        memcpy(b + 1, &hh, 2);
        memcpy(b + 3, &nn, 2);
        memcpy(b + 5, &end, 8);
        memcpy(b + 13, pts, n * sizeof(float));
        rec_put(REC_GPOINTS, pg, row, b, 13 + n * sizeof(float));
    }
    if (s_hold) return;
    if (s_graph_cb) s_graph_cb(pg, row, latest, unit, name, pts, n, end, hours, zero_based);
}

static void emit_button(int pg, int row, ha_btn_state_t st, const char *name)
{
    uint8_t b[40];
    b[0] = st;
    const char *a[] = { name };
    rec_put(REC_BUTTON, pg, row, b, 1 + pack_strs(b + 1, sizeof(b) - 1, a, 1));
    if (s_hold) return;
    if (s_button_cb) s_button_cb(pg, row, st, name);
}

static void emit_weather(int pg, int row, const char *cond, const char *temp, const char *unit,
                         const char *hum, const char *name)
{
    uint8_t b[96];
    const char *a[] = { cond, temp, unit, hum, name };
    rec_put(REC_WEATHER, pg, row, b, pack_strs(b, sizeof(b), a, 5));
    if (s_hold) return;
    if (s_weather_cb) s_weather_cb(pg, row, cond, temp, unit, hum, name);
}

static void emit_climate(int pg, int row, const ha_climate_t *c, const char *name)
{
    uint8_t b[160];
    const char *a[] = { c->mode, c->action, c->cur, c->target, c->min, c->max, c->step, name };
    rec_put(REC_CLIMATE, pg, row, b, pack_strs(b, sizeof(b), a, 8));
    if (s_hold) return;
    if (s_climate_cb) s_climate_cb(pg, row, c, name);
}

static void emit_media(int pg, int row, const char *st, const char *title, const char *artist, const char *name)
{
    uint8_t b[160];
    const char *a[] = { st, title, artist, name };
    rec_put(REC_MEDIA, pg, row, b, pack_strs(b, sizeof(b), a, 4));
    if (s_hold) return;
    if (s_media_cb) s_media_cb(pg, row, st, title, artist, name);
}

/* record: i8 pct, state, name */
static void emit_light(int pg, int row, const char *st, int pct, const char *name)
{
    uint8_t b[64];
    b[0] = (uint8_t)(int8_t)pct;
    const char *a[] = { st, name };
    rec_put(REC_LIGHT, pg, row, b, 1 + pack_strs(b + 1, sizeof(b) - 1, a, 2));
    if (s_hold) return;
    if (s_light_cb) s_light_cb(pg, row, st, pct, name);
}

/* record: u8 hourly, u8 n, n x ha_forecast_t */
static void emit_forecast(int pg, int row, const ha_forecast_t *f, int n, bool hourly)
{
    uint8_t b[2 + HA_FORECAST_MAX * sizeof(ha_forecast_t)];
    if (n < 0 || n > HA_FORECAST_MAX) return;
    b[0] = hourly; b[1] = n;
    memcpy(b + 2, f, n * sizeof(ha_forecast_t));
    rec_put(REC_FORECAST, pg, row, b, 2 + n * sizeof(ha_forecast_t));
    if (s_hold) return;
    if (s_forecast_cb) s_forecast_cb(pg, row, f, n, hourly);
}

/* record: u32 mask, i64 month, u8 n, n x cal_event_t */
static void emit_calendar(int pg, int row, const cal_event_t *ev, int n, uint32_t mask, int64_t month)
{
    static uint8_t b[13 + CAL_MAX_EVENTS * sizeof(cal_event_t)];   /* HA task only, under no lock */
    if (n < 0 || n > CAL_MAX_EVENTS) return;
    memcpy(b, &mask, 4);
    memcpy(b + 4, &month, 8);
    b[12] = n;
    memcpy(b + 13, ev, n * sizeof(cal_event_t));
    rec_put(REC_CALENDAR, pg, row, b, 13 + n * sizeof(cal_event_t));
    if (s_hold) return;
    if (s_calendar_cb) s_calendar_cb(pg, row, ev, n, mask, month);
}

static void emit_icon(int pg, int row, const char *icon)
{
    uint8_t b[52];
    const char *a[] = { icon };
    rec_put(REC_ICON, pg, row, b, pack_strs(b, sizeof(b), a, 1));
    if (s_hold) return;
    if (s_icon_cb) s_icon_cb(pg, row, icon);
}

/* blob: u8 theme, u8 brightness valid, 3 floats brightness, then records
 * (u8 type, u8 page, u8 row, u16 length, data); tagged with the layout hash */
static void cache_save(void)
{
    size_t n = 2 + sizeof(s_rec_bri);
    for (int p = 0; p < LAYOUT_MAX_PAGES; p++)
        for (int i = 0; i < LAYOUT_MAX_ROWS; i++)
            for (int t = 0; t < REC_TYPES; t++) if (s_rec[p][i][t]) n += 5 + s_rec_len[p][i][t];
    uint8_t *b = malloc(n), *o = b;
    if (!b) return;
    *o++ = s_rec_theme;
    *o++ = s_rec_bri_has;
    memcpy(o, s_rec_bri, sizeof(s_rec_bri));
    o += sizeof(s_rec_bri);
    for (int p = 0; p < LAYOUT_MAX_PAGES; p++)
        for (int i = 0; i < LAYOUT_MAX_ROWS; i++)
            for (int t = 0; t < REC_TYPES; t++) {
                if (!s_rec[p][i][t]) continue;
                uint16_t l = s_rec_len[p][i][t];
                *o++ = t; *o++ = p; *o++ = i;
                memcpy(o, &l, 2); o += 2;
                memcpy(o, s_rec[p][i][t], l); o += l;
            }
    if (cache_write(CACHE_DATA, b, n, s_layout_hash)) s_rec_dirty = false;
    free(b);
}

/* The saved data of this layout, through the callbacks (before HA is reachable) */
/* Send one record through its callback (d .. de = its data, l bytes) */
static void rec_emit(int t, int pg, int row, const uint8_t *d, const uint8_t *de, uint16_t l)
{
    switch (t) {
    case REC_VALUE: {
        const char *a = unpack_str(&d, de), *u = unpack_str(&d, de), *nm = unpack_str(&d, de);
        emit_sensor(pg, row, a, u, nm);
        break;
    }
    case REC_GLATEST: {
        const char *a = unpack_str(&d, de), *u = unpack_str(&d, de), *nm = unpack_str(&d, de);
        emit_graph(pg, row, a, u, nm, NULL, 0, 0, s_layout.pages[pg].items[row].hours, false);
        break;
    }
    case REC_GPOINTS: {
        if (l < 13) break;
        uint16_t hh, nn;
        int64_t e;
        memcpy(&hh, d + 1, 2); memcpy(&nn, d + 3, 2); memcpy(&e, d + 5, 8);
        if (nn > HA_GRAPH_POINTS || l != 13 + nn * sizeof(float)) break;
        static float pts[HA_GRAPH_POINTS];
        memcpy(pts, d + 13, nn * sizeof(float));
        emit_graph(pg, row, NULL, NULL, NULL, pts, nn, e, hh, d[0]);
        break;
    }
    case REC_BUTTON: {
        if (l < 1) break;
        const uint8_t *q = d + 1;
        emit_button(pg, row, (ha_btn_state_t)d[0], unpack_str(&q, de));
        break;
    }
    case REC_WEATHER: {
        const char *c = unpack_str(&d, de), *tp = unpack_str(&d, de), *u = unpack_str(&d, de);
        const char *h = unpack_str(&d, de), *nm = unpack_str(&d, de);
        emit_weather(pg, row, c, tp, u, h, nm);
        break;
    }
    case REC_FORECAST: {
        if (l < 2 || d[1] > HA_FORECAST_MAX || l != 2 + d[1] * sizeof(ha_forecast_t)) break;
        ha_forecast_t f[HA_FORECAST_MAX];
        memcpy(f, d + 2, d[1] * sizeof(ha_forecast_t));
        emit_forecast(pg, row, f, d[1], d[0]);
        break;
    }
    case REC_CALENDAR: {
        if (l < 13 || d[12] > CAL_MAX_EVENTS || l != 13 + d[12] * sizeof(cal_event_t)) break;
        static cal_event_t ev[CAL_MAX_EVENTS];   /* off the stack: replay runs at boot */
        uint32_t mask;
        int64_t month;
        int n = d[12];
        memcpy(&mask, d, 4);
        memcpy(&month, d + 4, 8);
        memcpy(ev, d + 13, n * sizeof(cal_event_t));
        for (int k = 0; k < n; k++) ev[k].summary[CAL_SUMMARY_LEN - 1] = 0;
        emit_calendar(pg, row, ev, n, mask, month);
        break;
    }
    case REC_CLIMATE: {
        ha_climate_t c;
        char *f[] = { c.mode, c.action, c.cur, c.target, c.min, c.max, c.step };
        size_t sz[] = { sizeof(c.mode), sizeof(c.action), sizeof(c.cur), sizeof(c.target), sizeof(c.min), sizeof(c.max), sizeof(c.step) };
        for (int k = 0; k < 7; k++) snprintf(f[k], sz[k], "%s", unpack_str(&d, de));
        emit_climate(pg, row, &c, unpack_str(&d, de));
        break;
    }
    case REC_MEDIA: {
        const char *st = unpack_str(&d, de), *ti = unpack_str(&d, de), *ar = unpack_str(&d, de);
        emit_media(pg, row, st, ti, ar, unpack_str(&d, de));
        break;
    }
    case REC_LIGHT: {
        if (l < 1) break;
        const uint8_t *q = d + 1;
        const char *st = unpack_str(&q, de);
        emit_light(pg, row, st, (int8_t)d[0], unpack_str(&q, de));
        break;
    }
    case REC_ICON: {
        const char *ic = unpack_str(&d, de);
        snprintf(s_icon_last[pg][row], sizeof(s_icon_last[pg][row]), "%s", ic);
        emit_icon(pg, row, ic);
        break;
    }
    }
}

static void cache_replay(void)
{
    size_t n;
    uint32_t tag;
    uint8_t *b = cache_read(CACHE_DATA, &n, &tag);
    if (!b) return;
    if (tag != s_layout_hash || n < 2 + sizeof(s_rec_bri)) { free(b); return; }
    const uint8_t *p = b, *end = b + n;
    uint8_t theme = *p++, bri_has = *p++;
    float bri[3];
    memcpy(bri, p, sizeof(bri));
    p += sizeof(bri);
    if ((theme == HA_STATE_ON || theme == HA_STATE_OFF) && s_state_cb) {
        s_rec_theme = theme;
        s_state_cb(theme);
        s_state_cb(HA_STATE_OFFLINE);    /* the theme only: not connected yet */
    }
    if (bri_has && s_brightness_cb) { memcpy(s_rec_bri, bri, sizeof(bri)); s_rec_bri_has = true; s_brightness_cb(bri[0], bri[1], bri[2]); }
    int count = 0;
    while (end - p >= 5) {
        int t = p[0], pg = p[1], row = p[2];
        uint16_t l;
        memcpy(&l, p + 3, 2);
        p += 5;
        if (l > end - p) break;
        const uint8_t *d = p, *de = p + l;
        p += l;
        if (t >= REC_TYPES || pg >= s_layout.count || row >= s_layout.pages[pg].count) continue;
        count++;
        rec_emit(t, pg, row, d, de, l);
    }
    free(b);
    s_rec_dirty = false;
    ESP_LOGI(TAG, "cache: %d cards shown from flash", count);
}

static float read_entity(int pg, int row, char text[24], char unit[8], char name[32], bool *got);
static float parse_entity(int pg, int row, const char *json, char text[24], char unit[8], char name[32]);
static void parse_button(int pg, int row, const char *json);
static void parse_weather(int pg, int row, const char *json);
static void report_icon(int pg, int row, const char *json);
static void utf8_trim(char *s);
static void fetch_weather(int pg, int row);
static void fetch_forecast(int pg, int row);
static void fetch_calendar(int pg, int row);
static void fetch_climate(int pg, int row);
static void fetch_media(int pg, int row);
static void fetch_light(int pg, int row);
static void fetch_todo(int pg, int row);
static dash_ws_t *s_lws;          /* the live websocket, NULL when closed */
static void parse_light(int pg, int row, const char *json);
static void parse_media(int pg, int row, const char *json);
static void parse_climate(int pg, int row, const char *json);
static void fetch_button(int pg, int row);

/* A tap/toggle is waiting: background reads (graph history, pictures, the rest of a poll)
 * stop so it reaches HA at once; they are picked up again on the next poll */
static bool user_waiting(void)
{
    return xEventGroupGetBits(s_events) & (TOGGLE_BIT | ORIENT_BIT | SETBRI_BIT | PRESS_BIT | TARGET_BIT | MEDIA_BIT | LIGHT_BIT | TODO_BIT | PRES_BIT | NIGHT_BIT | AUTOROT_BIT | MOTION_BIT | SAVER_BIT);
}

static void parse_brightness(const char *json)
{
    float v, mn = 0, mx = 100;
    if (!json_num(json, "\"state\":\"", &v)) return;
    json_num(json, "\"min\":", &mn);
    json_num(json, "\"max\":", &mx);
    static float lv = -1e9f, lmn, lmx;
    if ((s_force_report || v != lv || mn != lmn || mx != lmx) && s_brightness_cb) s_brightness_cb(v, mn, mx);
    if (!s_rec_bri_has || s_rec_bri[0] != v || s_rec_bri[1] != mn || s_rec_bri[2] != mx) {
        s_rec_bri[0] = v; s_rec_bri[1] = mn; s_rec_bri[2] = mx;
        s_rec_bri_has = s_rec_dirty = true;
    }
    lv = v; lmn = mn; lmx = mx;
    s_force_report = false;
}

static void board_parse(const char *json, char *out, size_t n)
{
    char st[64] = "";
    json_str(json, "\"state\":\"", st, sizeof(st));
    if (!strcmp(st, "unknown") || !strcmp(st, "unavailable")) st[0] = 0;
    board_slug(st, out, n);
}

static int ha_service(const char *path, const char *body);

/* The portal's name goes to the name helper once, if HA has it; then the helper leads again */
static void name_push_done(void)
{
    s_name_push = false;
    nvs_handle_t h;
    if (nvs_open("board", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "push");
        nvs_commit(h);
        nvs_close(h);
    }
}

/* After setup: create the board's missing helpers over the websocket API, starting with the
 * board's own values, so the board hands those settings to HA. Only with an admin token
 * (creating helpers needs one); otherwise skipped. Runs once (retried if HA didn't answer). */
/* For the log, indexed by H_*: one per helper, or the build fails */
static const char *const HELPER_NAME[] = {
    "theme", "landscape", "presentation", "presentation interval", "brightness", "dashboard", "view", "page",
    "night", "night brightness", "screen timeout", "notification", "auto rotate", "motion wake", "screensaver",
};
_Static_assert(sizeof(HELPER_NAME) / sizeof(HELPER_NAME[0]) == H_COUNT, "a name for every helper");

static void create_helpers(void)
{
    bool any = false;
    for (int h = 0; h < H_COUNT; h++) any |= s_has[h] == 0;
    if (!any || !s_board[0]) { s_mkhelp = false; goto done; }
    ha_local_t L = { .dark = true, .pres_secs = 10, .bri_pct = 100 };
    if (s_local_cb) s_local_cb(&L);
    if (s_http) {                         /* two TLS sessions at once don't fit in RAM */
        esp_http_client_cleanup(s_http);
        s_http = NULL;
    }
    dash_ws_t *d = dash_ws_open();
    if (!d) return;                       /* HA didn't answer: next round */
    s_mkhelp = false;
    char *u = dash_ws_call(d, "\"type\":\"auth/current_user\"");
    bool admin = u && strstr(u, "\"is_admin\":true");
    free(u);
    if (!admin) {
        ESP_LOGI(TAG, "helpers: the token's user isn't an HA admin, not creating them");
        dash_ws_close(d);
        goto done;
    }
    const char *b = s_board;
    char body[320];
    for (int h = 0; h < H_COUNT; h++) {
        if (s_has[h] != 0) continue;
        switch (h) {
        case H_THEME: snprintf(body, sizeof(body), "\"type\":\"input_boolean/create\",\"name\":\"%s theme\",\"icon\":\"mdi:theme-light-dark\",\"initial\":%s", b, L.dark ? "true" : "false"); break;
        case H_LAND: snprintf(body, sizeof(body), "\"type\":\"input_boolean/create\",\"name\":\"%s landscape\",\"icon\":\"mdi:phone-rotate-landscape\",\"initial\":%s", b, L.landscape ? "true" : "false"); break;
        case H_PRES: snprintf(body, sizeof(body), "\"type\":\"input_boolean/create\",\"name\":\"%s presentation\",\"icon\":\"mdi:presentation-play\",\"initial\":%s", b, L.pres_on ? "true" : "false"); break;
        case H_PRES_INT: snprintf(body, sizeof(body), "\"type\":\"input_number/create\",\"name\":\"%s presentation interval\",\"min\":1,\"max\":600,\"step\":1,\"mode\":\"box\",\"unit_of_measurement\":\"s\",\"initial\":%d", b, L.pres_secs); break;
        case H_BRI: snprintf(body, sizeof(body), "\"type\":\"input_number/create\",\"name\":\"%s brightness\",\"min\":0,\"max\":100,\"step\":1,\"mode\":\"slider\",\"unit_of_measurement\":\"%%\",\"initial\":%d", b, L.bri_pct); break;
        case H_PAGE: snprintf(body, sizeof(body), "\"type\":\"input_number/create\",\"name\":\"%s page\",\"min\":1,\"max\":8,\"step\":1,\"mode\":\"box\",\"initial\":%d", b, L.page + 1); break;
        case H_DASH: snprintf(body, sizeof(body), "\"type\":\"input_text/create\",\"name\":\"%s dashboard\",\"max\":64,\"initial\":\"%s\"", b, L.dash); break;
        case H_VIEW: snprintf(body, sizeof(body), "\"type\":\"input_text/create\",\"name\":\"%s view\",\"max\":64,\"initial\":\"%s\"", b, L.view); break;
        case H_NIGHT: snprintf(body, sizeof(body), "\"type\":\"input_boolean/create\",\"name\":\"%s night\",\"icon\":\"mdi:weather-night\",\"initial\":%s", b, L.night ? "true" : "false"); break;
        case H_NIGHT_BRI: snprintf(body, sizeof(body), "\"type\":\"input_number/create\",\"name\":\"%s night brightness\",\"min\":0,\"max\":100,\"step\":1,\"mode\":\"slider\",\"unit_of_measurement\":\"%%\",\"initial\":%d", b, L.night_pct); break;
        case H_SCR_TO: snprintf(body, sizeof(body), "\"type\":\"input_number/create\",\"name\":\"%s screen timeout\",\"min\":0,\"max\":3600,\"step\":5,\"mode\":\"box\",\"unit_of_measurement\":\"s\",\"icon\":\"mdi:timer-off-outline\",\"initial\":%d", b, L.timeout_s); break;
        case H_AUTOROT: snprintf(body, sizeof(body), "\"type\":\"input_boolean/create\",\"name\":\"%s auto rotate\",\"icon\":\"mdi:screen-rotation\",\"initial\":%s", b, L.auto_rot ? "true" : "false"); break;
        case H_MOTION: snprintf(body, sizeof(body), "\"type\":\"input_boolean/create\",\"name\":\"%s motion wake\",\"icon\":\"mdi:gesture-tap-hold\",\"initial\":%s", b, L.motion ? "true" : "false"); break;
        case H_SAVER: snprintf(body, sizeof(body), "\"type\":\"input_boolean/create\",\"name\":\"%s screensaver\",\"icon\":\"mdi:clock-outline\",\"initial\":%s", b, L.saver ? "true" : "false"); break;
        case H_NOTIFY: snprintf(body, sizeof(body), "\"type\":\"input_text/create\",\"name\":\"%s notification\",\"max\":255,\"icon\":\"mdi:message-alert\",\"initial\":\"\"", b); break;
        }
        char *res = dash_ws_call(d, body);
        bool ok = res && strstr(res, "\"success\":true");
        ESP_LOGI(TAG, "helpers: create %s %s: %s", b, HELPER_NAME[h], ok ? "ok" : res ? res : "no answer");
        if (ok) s_has[h] = 1;
        free(res);
    }
    dash_ws_close(d);
    s_force_report = true;                /* the new helpers' values on the next round */
done:
    {
        nvs_handle_t nh;
        if (nvs_open("board", NVS_READWRITE, &nh) == ESP_OK) {
            nvs_erase_key(nh, "mkhelp");
            nvs_set_u8(nh, "helpver", HELPERS_VERSION);
            nvs_commit(nh);
            nvs_close(nh);
        }
    }
}

static bool fetch_name(void)
{
    resp_t *r = &s_rbuf;   /* shared: all callers run on the HA task, one at a time */
    char path[80], name[32] = "";
    snprintf(path, sizeof(path), "/api/states/%s", s_name_ent);
    s_optional = true;
    int st = ha_request(HTTP_METHOD_GET, path, NULL, r);
    s_optional = false;
    if (st == 404 && s_name_push) name_push_done();   /* no helper: nothing to write */
    if (st != 200) return false;          /* no helper (404) or no answer: keep the name */
    if (s_name_push) {
        char body[160];
        snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",\"value\":\"%s\"}", s_name_ent,
                 s_board_raw[0] ? s_board_raw : s_board);
        int ws = ha_service("/api/services/input_text/set_value", body);
        ESP_LOGI(TAG, "name '%s' -> %s: %d", s_board_raw, s_name_ent, ws);
        if (ws >= 200 && ws < 500) name_push_done();   /* done, or rejected: don't retry */
        return false;                     /* the board's name stands */
    }
    board_parse(r->buf, name, sizeof(name));
    if (!name[0]) return false;           /* helper empty: keep the name from the portal */
    return board_set(name);
}

/* GET a board helper; 0 while the board has no name (helpers aren't read then) */
static int helper_get(const char *id, resp_t *r, int h)
{
    if (!s_board[0]) return 0;
    char path[112];
    snprintf(path, sizeof(path), "/api/states/%s", id);
    s_optional = true;
    int st = ha_request(HTTP_METHOD_GET, path, NULL, r);
    s_optional = false;
    if (st == 200) s_has[h] = 1;
    else if (st == 404) s_has[h] = 0;
    return st;
}

static bool fetch_state(void)
{
    resp_t *r = &s_rbuf;   /* shared: all callers run on the HA task, one at a time */
    int st = helper_get(s_ent.theme, r, H_THEME);
    ha_state_t s = HA_STATE_UNKNOWN;
    if (st == 200) {
        if (strstr(r->buf, "\"state\":\"on\"")) s = HA_STATE_ON;
        else if (strstr(r->buf, "\"state\":\"off\"")) s = HA_STATE_OFF;
    } else if (st == 404) {               /* no helper: the board keeps its own theme */
        s = HA_STATE_LOCAL;
    } else if (st != 0) {
        s = HA_STATE_ERROR;
    }
    if ((s == HA_STATE_ON || s == HA_STATE_OFF) && s != s_rec_theme) { s_rec_theme = s; s_rec_dirty = true; }
    if (s_state_cb) s_state_cb(s);

    if (helper_get(s_ent.land, r, H_LAND) == 200) {
        bool on = strstr(r->buf, "\"state\":\"on\"") != NULL;
        bool off = strstr(r->buf, "\"state\":\"off\"") != NULL;
        if ((on || off) && s_land_cb) s_land_cb(on);
    }

    /* Presentation mode: on/off and seconds per page */
    {
        int pon = -1;
        float secs = 0;
        if (helper_get(s_ent.pres, r, H_PRES) == 200) {
            if (strstr(r->buf, "\"state\":\"on\"")) pon = 1;
            else if (strstr(r->buf, "\"state\":\"off\"")) pon = 0;
        }
        if (helper_get(s_ent.pres_int, r, H_PRES_INT) == 200)
            json_num(r->buf, "\"state\":\"", &secs);
        if (pon >= 0 && s_pres_cb) s_pres_cb(pon, (int)secs);
    }

    {   /* night mode helpers; -1 = no helper, the board keeps its own */
        int night = -1;
        float pct = -1, to = -1;
        if (helper_get(s_ent.night, r, H_NIGHT) == 200) {
            if (strstr(r->buf, "\"state\":\"on\"")) night = 1;
            else if (strstr(r->buf, "\"state\":\"off\"")) night = 0;
        }
        if (helper_get(s_ent.night_bri, r, H_NIGHT_BRI) != 200 || !json_num(r->buf, "\"state\":\"", &pct)) pct = -1;
        if (helper_get(s_ent.scr_to, r, H_SCR_TO) != 200 || !json_num(r->buf, "\"state\":\"", &to)) to = -1;
        s_ln_night = night; s_ln_pct = (int)(pct + 0.5f); s_ln_to = (int)(to + 0.5f);
        if (pct < 0) s_ln_pct = -1;
        if (to < 0) s_ln_to = -1;
        if (s_night_cb) s_night_cb(s_ln_night, s_ln_pct, s_ln_to);
    }

    if (helper_get(s_ent.autorot, r, H_AUTOROT) == 200 && s_autorot_cb) {
        if (strstr(r->buf, "\"state\":\"on\"")) s_autorot_cb(true);
        else if (strstr(r->buf, "\"state\":\"off\"")) s_autorot_cb(false);
    }
    if (helper_get(s_ent.motion, r, H_MOTION) == 200 && s_motion_cb) {
        if (strstr(r->buf, "\"state\":\"on\"")) s_motion_cb(true);
        else if (strstr(r->buf, "\"state\":\"off\"")) s_motion_cb(false);
    }
    if (helper_get(s_ent.saver, r, H_SAVER) == 200 && s_saver_cb) {
        if (strstr(r->buf, "\"state\":\"on\"")) s_saver_cb(true);
        else if (strstr(r->buf, "\"state\":\"off\"")) s_saver_cb(false);
    }

    if (s_has[H_NOTIFY] < 0) helper_get(s_ent.notify, r, H_NOTIFY);   /* only whether it's there: messages come live */

    /* Brightness: input_number state like "42.0" plus its min/max attributes */
    if (helper_get(s_ent.bri, r, H_BRI) == 200) parse_brightness(r->buf);

    /* Dashboard / view / page */
    s_nav_dash_ok = helper_get(s_ent.dash, r, H_DASH) == 200 &&
                    nav_text(r->buf, s_nav_dash, sizeof(s_nav_dash));
    s_nav_view_ok = helper_get(s_ent.view, r, H_VIEW) == 200 &&
                    nav_text(r->buf, s_nav_view, sizeof(s_nav_view));
    s_nav_page = helper_get(s_ent.page, r, H_PAGE) == 200 ? nav_page(r->buf) : -1;
    nav_report();

    for (int pg = 0; pg < s_layout.count && !user_waiting(); pg++) {
        for (int i = 0; i < s_layout.pages[pg].count && !user_waiting(); i++) {
            const layout_item_t *it = &s_layout.pages[pg].items[i];
            if (it->kind == LAYOUT_BUTTONS) { fetch_button(pg, i); continue; }
            if (it->kind == LAYOUT_WEATHER) { fetch_weather(pg, i); continue; }
            if (it->kind == LAYOUT_CALENDAR) { fetch_calendar(pg, i); continue; }
            if (it->kind == LAYOUT_THERMOSTAT) { fetch_climate(pg, i); continue; }
            if (it->kind == LAYOUT_MEDIA) { fetch_media(pg, i); continue; }
            if (it->kind == LAYOUT_LIGHT) { fetch_light(pg, i); continue; }
            if (it->kind == LAYOUT_TODO) { fetch_todo(pg, i); continue; }
            if (it->kind != LAYOUT_VALUES && it->kind != LAYOUT_GAUGE && it->kind != LAYOUT_GLANCE &&
                it->kind != LAYOUT_ENTITIES) continue;   /* graphs: fetch_graph; clocks: no data */
            char text[24], unit[8], name[32];
            bool got;
            read_entity(pg, i, text, unit, name, &got);
            if (got) emit_sensor(pg, i, text, unit, name);   /* failed: keep the last value */
        }
    }
    return st == 200 || st == 404 || st == 0;
}

/* Copies the JSON string value after `key` ("\"key\":\"") into out; false if missing */
static bool json_str(const char *json, const char *key, char *out, size_t n)
{
    const char *p = strstr(json, key);
    if (!p) return false;
    p += strlen(key);
    size_t i = 0;
    while (p[i] && p[i] != '"' && i < n - 1) { out[i] = p[i]; i++; }
    out[i] = 0;
    return true;
}

/* Current value of a layout item as display text ("--" when not numeric), its unit and the
 * name to show (card name, else friendly_name, else the entity id). Returns the value, or NAN.
 * *got = false when HA didn't answer: the caller keeps showing the last value. */
static float read_entity(int pg, int row, char text[24], char unit[8], char name[32], bool *got)
{
    const layout_item_t *it = &s_layout.pages[pg].items[row];
    resp_t *r = &s_rbuf;   /* shared: all callers run on the HA task, one at a time */
    char path[96];
    float v = NAN;
    strcpy(text, "--");
    unit[0] = 0;
    snprintf(name, 32, "%.31s", it->name[0] ? it->name : it->entity);
    snprintf(path, sizeof(path), "/api/states/%s", it->entity);
    *got = ha_request(HTTP_METHOD_GET, path, NULL, r) == 200;
    if (*got) v = parse_entity(pg, row, r->buf, text, unit, name);
    return v;
}

/* The value of row `row` from its /api/states JSON (or the same built from a live event) */
static float parse_entity(int pg, int row, const char *json, char text[24], char unit[8], char name[32])
{
    const layout_item_t *it = &s_layout.pages[pg].items[row];
    float v = NAN;
    strcpy(text, "--");
    unit[0] = 0;
    snprintf(name, 32, "%.31s", it->name[0] ? it->name : it->entity);
    report_icon(pg, row, json);
    if (!it->name[0] && json_str(json, "\"friendly_name\":\"", name, 32)) utf8_trim(name);
    json_str(json, "\"unit_of_measurement\":\"", unit, 8);
    char st[48];
    if (json_num(json, "\"state\":\"", &v)) {
        snprintf(text, 24, "%.*f", fabsf(v) < 100 ? 1 : 0, (double)v);   /* 1 decimal below 100 */
    } else {
        v = NAN;
        /* text states ("not_charging" -> "Not charging"); no unit for them */
        if (json_str(json, "\"state\":\"", st, sizeof(st)) && strcmp(st, "unavailable") && strcmp(st, "unknown")) {
            state_text(st, text, 24);
            unit[0] = 0;
        }
    }
    return v;
}

/* Graph page: latest value every poll, then the history over the page's span */
static void graph_emit_buckets(int pg, int row, int hours, bool zero_based, bool hold);

/* statistics-graph (issue #52): the item's long-term statistics over the websocket
 * (recorder/statistics_during_period) into the history buckets. False if none came. */
static bool fetch_stats(const layout_item_t *it)
{
    static const char *const PERIOD[] = { "hour", "5minute", "day", "week", "month" };
    static const char *const TYPE[] = { "mean", "min", "max", "state" };
    int per = (it->flags & LAYOUT_GRAPH_PERIOD) >> 1, ty = (it->flags & LAYOUT_GRAPH_TYPE) >> 4;
    if (per > 4) per = 0;
    /* at most HA_GRAPH_POINTS rows: the graph has no more columns, and the answer must stay
     * under the websocket's 32 KB (30 days of hours would not) -> a coarser period */
    static const int ORDER[] = { 1, 0, 2, 3, 4 };              /* 5minute, hour, day, week, month */
    static const int64_t SECS[] = { 3600, 300, 86400, 604800, 2592000 };
    int o = 0;
    while (ORDER[o] != per) o++;
    while (o < 4 && (int64_t)it->hours * 3600 / SECS[ORDER[o]] > HA_GRAPH_POINTS) o++;
    if (ORDER[o] != per) ESP_LOGI(TAG, "statistics of %s: period %s instead of %s (%d h)", it->entity, PERIOD[ORDER[o]], PERIOD[per], it->hours);
    per = ORDER[o];
    char start[32], stop[32], body[320];
    time_t t0 = (time_t)(s_now - (int64_t)it->hours * 3600), t1 = (time_t)s_now;
    struct tm g;
    gmtime_r(&t0, &g);
    strftime(start, sizeof(start), "%Y-%m-%dT%H:%M:%SZ", &g);
    gmtime_r(&t1, &g);
    strftime(stop, sizeof(stop), "%Y-%m-%dT%H:%M:%SZ", &g);
    snprintf(body, sizeof(body), "\"type\":\"recorder/statistics_during_period\",\"start_time\":\"%s\",\"end_time\":\"%s\","
             "\"statistic_ids\":[\"%s\"],\"period\":\"%s\",\"types\":[\"%s\"]", start, stop, it->entity, PERIOD[per], TYPE[ty]);
    if (s_http) {                         /* two TLS sessions at once don't fit in RAM */
        esp_http_client_cleanup(s_http);
        s_http = NULL;
    }
    dash_ws_t *d = dash_ws_open();
    char *msg = d ? dash_ws_call(d, body) : NULL;
    dash_ws_close(d);
    if (!msg) { ESP_LOGW(TAG, "statistics of %s: no answer", it->entity); return false; }
    cJSON *root = cJSON_Parse(msg);
    free(msg);
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "result"), it->entity), *e;
    int n = 0;
    cJSON_ArrayForEach(e, list) {          /* {"start": ms, "end": ms, "<type>": value} */
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(e, "start"), *v = cJSON_GetObjectItemCaseSensitive(e, TYPE[ty]);
        if (!cJSON_IsNumber(st) || !cJSON_IsNumber(v)) continue;
        hist_add((int64_t)(st->valuedouble / 1000), (float)v->valuedouble);
        n++;
    }
    cJSON_Delete(root);
    if (!n) ESP_LOGW(TAG, "statistics of %s: none (not a statistics sensor?)", it->entity);
    return n > 0;
}

static void fetch_graph(int pg, int row)
{
    const layout_item_t *it = &s_layout.pages[pg].items[row];
    char latest[24], unit[8], name[32], path[256];
    bool got;
    read_entity(pg, row, latest, unit, name, &got);
    bool zero_based = !strcmp(unit, "%");
    if (got) emit_graph(pg, row, latest, unit, name, NULL, 0, 0, it->hours, zero_based);

    if (!s_now) return;                   /* need the server time to place points */
    s_hist_hours = it->hours;
    memset(s_hist_has, 0, sizeof(s_hist_has));
    if (it->flags & LAYOUT_GRAPH_STATS) {
        if (!fetch_stats(it)) return;
        graph_emit_buckets(pg, row, it->hours, zero_based, false);   /* one value per period */
        return;
    }

    /* start time = now - span, as "2026-11-19T13:05:12+00:00" */
    time_t t0 = (time_t)(s_now - (int64_t)it->hours * 3600);
    struct tm g;
    gmtime_r(&t0, &g);
    char start[32], stop[32];
    strftime(start, sizeof(start), "%Y-%m-%dT%H:%M:%S%%2B00:00", &g);
    time_t t1 = (time_t)s_now;   /* without end_time HA returns only one day */
    gmtime_r(&t1, &g);
    strftime(stop, sizeof(stop), "%Y-%m-%dT%H:%M:%S%%2B00:00", &g);
    s_hist_len = 0;
    s_hist_mode = true;
    snprintf(path, sizeof(path), "/api/history/period/%s?end_time=%s&filter_entity_id=%s&minimal_response&no_attributes",
             start, stop, it->entity);
    int st = ha_request(HTTP_METHOD_GET, path, NULL, NULL);
    s_hist_mode = false;
    if (st != 200) return;

    graph_emit_buckets(pg, row, it->hours, zero_based, true);   /* states hold until they change */
}

/* The filled buckets as the graph's points: a smooth curve through the buckets with data
 * (monotone, so no overshoot), like HA's charts. A bucket shows its peak. hold (history): a
 * state holds until it changes, so the value the sensor ended a bucket on is also put just
 * before the next change (and right after a peak): flat, then a smooth bend into the change,
 * and a short spike falls back at once instead of over hours. */
static void graph_emit_buckets(int pg, int row, int hours, bool zero_based, bool hold)
{
    static float pts[HA_GRAPH_POINTS];
    static bool has[HA_GRAPH_POINTS];
    _Static_assert(HA_GRAPH_POINTS <= CURVE_MAX, "curve_fill takes the whole graph");
    int known = 0;
    for (int i = 0; i < HA_GRAPH_POINTS; i++) { pts[i] = s_hist_bucket[i]; has[i] = s_hist_has[i]; known += has[i]; }
    for (int i = 0; hold && i < HA_GRAPH_POINTS; i++) {
        if (!s_hist_has[i]) continue;
        int j = i + 1;
        while (j < HA_GRAPH_POINTS && !s_hist_has[j]) j++;
        if (j == i + 1) continue;          /* the next bucket changes already */
        pts[i + 1] = s_hist_last[i];       /* back from the peak to where it ended */
        has[i + 1] = true;
        if (j < HA_GRAPH_POINTS && j - 1 > i + 1) { pts[j - 1] = s_hist_last[i]; has[j - 1] = true; }   /* held until the change */
    }
    curve_fill(pts, has, HA_GRAPH_POINTS);
    ESP_LOGD(TAG, "graph %dh: %d of %d buckets with data", hours, known, HA_GRAPH_POINTS);
    emit_graph(pg, row, NULL, NULL, NULL, pts, HA_GRAPH_POINTS, s_now, hours, zero_based);
}

static bool is_domain(const char *entity, const char *domain)
{
    size_t n = strlen(domain);
    return !strncmp(entity, domain, n) && entity[n] == '.';
}

/* todo-list card (issue #55): todo.get_items with its response, over the live websocket when
 * open, else REST (?return_response). Open items first, then the completed ones. */
static void fetch_todo(int pg, int row)
{
    const char *e = s_layout.pages[pg].items[row].entity;
    char body[256];
    char *msg = NULL;
    if (dash_ws_alive(s_lws)) {
        snprintf(body, sizeof(body), "\"type\":\"call_service\",\"domain\":\"todo\",\"service\":\"get_items\","
                 "\"service_data\":{\"entity_id\":\"%s\"},\"return_response\":true", e);
        msg = dash_ws_call(s_lws, body);
    } else {
        resp_t *r = &s_rbuf;   /* shared: all callers run on the HA task, one at a time */
        snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", e);
        r->len = 0;
        if (ha_request(HTTP_METHOD_POST, "/api/services/todo/get_items?return_response", body, r) == 200) msg = strdup(r->buf);
    }
    if (!msg) return;                     /* keep what is shown */
    cJSON *root = cJSON_Parse(msg);
    free(msg);
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(root, "service_response");   /* REST */
    if (!res) res = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "result"), "response");   /* websocket */
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(res, e), "items"), *it;
    if (!cJSON_IsArray(items)) { cJSON_Delete(root); return; }
    static ha_todo_item_t out[HA_TODO_MAX];
    int n = 0;
    for (int pass = 0; pass < 2; pass++)  /* open, then completed */
        cJSON_ArrayForEach(it, items) {
            if (n == HA_TODO_MAX) break;
            const cJSON *st = cJSON_GetObjectItemCaseSensitive(it, "status"), *uid = cJSON_GetObjectItemCaseSensitive(it, "uid"),
                        *sum = cJSON_GetObjectItemCaseSensitive(it, "summary");
            if (!cJSON_IsString(uid) || !cJSON_IsString(sum)) continue;
            bool done = cJSON_IsString(st) && !strcmp(st->valuestring, "completed");
            if (done != (pass == 1)) continue;
            snprintf(out[n].uid, sizeof(out[n].uid), "%s", uid->valuestring);
            snprintf(out[n].summary, sizeof(out[n].summary), "%s", sum->valuestring);
            utf8_trim(out[n].summary);
            out[n].done = done;
            n++;
        }
    cJSON_Delete(root);
    if (s_hold) return;
    if (s_todo_cb) s_todo_cb(pg, row, out, n);
}

/* Button state and display name */
static void fetch_button(int pg, int row)
{
    resp_t *r = &s_rbuf;   /* shared: all callers run on the HA task, one at a time */
    char path[96];
    snprintf(path, sizeof(path), "/api/states/%s", s_layout.pages[pg].items[row].entity);
    if (ha_request(HTTP_METHOD_GET, path, NULL, r) != 200) return;   /* keep the last state */
    parse_button(pg, row, r->buf);
}

static void parse_button(int pg, int row, const char *json)
{
    const layout_item_t *it = &s_layout.pages[pg].items[row];
    char state[24] = "", name[32];
    snprintf(name, sizeof(name), "%.31s", it->name[0] ? it->name : it->entity);
    ha_btn_state_t bs = HA_BTN_UNAVAILABLE;
    {
        report_icon(pg, row, json);
        if (!it->name[0] && json_str(json, "\"friendly_name\":\"", name, sizeof(name))) utf8_trim(name);
        json_str(json, "\"state\":\"", state, sizeof(state));
        if (!strcmp(state, "unavailable")) bs = HA_BTN_UNAVAILABLE;
        else if (is_domain(it->entity, "script")) bs = HA_BTN_SCRIPT;
        else if (is_domain(it->entity, "scene")) bs = HA_BTN_SCENE;
        else if (is_domain(it->entity, "button") || is_domain(it->entity, "input_button")) bs = HA_BTN_PRESS;
        else if (!strcmp(state, "on")) bs = HA_BTN_ON;
        else if (!strcmp(state, "off")) bs = HA_BTN_OFF;
    }
    emit_button(pg, row, bs, name);
}

/* Toggle / run / activate the tapped button's entity */
static int ha_service(const char *path, const char *body);
static bool press_button(int pg, int row)
{
    if (pg < 0 || pg >= s_layout.count || row < 0 || row >= s_layout.pages[pg].count ||
        s_layout.pages[pg].items[row].kind != LAYOUT_BUTTONS) return false;
    const layout_item_t *it = &s_layout.pages[pg].items[row];
    if (it->action != LAYOUT_ACT_TOGGLE) { ESP_LOGW(TAG, "%s: tap action not supported", it->entity); return false; }
    const char *svc = is_domain(it->entity, "script") ? "/api/services/script/turn_on"
                    : is_domain(it->entity, "scene")  ? "/api/services/scene/turn_on"
                    : is_domain(it->entity, "button") ? "/api/services/button/press"
                    : is_domain(it->entity, "input_button") ? "/api/services/input_button/press"
                    : "/api/services/homeassistant/toggle";
    char body[96];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", it->entity);
    ESP_LOGI(TAG, "press %s -> %s", it->entity, svc);
    int st = ha_service(svc, body);
    return st >= 200 && st < 300;
}

/* Value entities may be anything (text states are shown as text); graph entities must be
 * numeric (or unavailable/unknown); button entities must be
 * on/off (or unavailable/unknown), or a script, scene, button or input_button (pressed).
 * Missing entities are dropped;
 * network errors keep the entity. */
static bool entity_usable(const char *entity, layout_kind_t kind, void *ctx)
{
    (void)ctx;
    resp_t *r = &s_rbuf;   /* shared: all callers run on the HA task, one at a time */
    char path[96], state[24];
    snprintf(path, sizeof(path), "/api/states/%s", entity);
    int st = ha_request(HTTP_METHOD_GET, path, NULL, r);
    if (st == 404) { ESP_LOGW(TAG, "layout: %s does not exist, skipped", entity); return false; }
    if (st != 200 || !json_str(r->buf, "\"state\":\"", state, sizeof(state))) return true;
    if (!strcmp(state, "unavailable") || !strcmp(state, "unknown")) return true;
    if (kind == LAYOUT_BUTTONS) {
        if (is_domain(entity, "script") || is_domain(entity, "scene") ||
            is_domain(entity, "button") || is_domain(entity, "input_button") ||   /* state = last press time */
            !strcmp(state, "on") || !strcmp(state, "off")) return true;
        ESP_LOGW(TAG, "layout: %s can't be switched ('%s'), skipped", entity, state);
        return false;
    }
    if (kind == LAYOUT_VALUES || kind == LAYOUT_GLANCE || kind == LAYOUT_ENTITIES) return true;   /* text states are shown as text */
    if (kind == LAYOUT_WEATHER) {
        if (is_domain(entity, "weather")) return true;
        ESP_LOGW(TAG, "layout: %s is not a weather entity, skipped", entity);
        return false;
    }
    if (kind == LAYOUT_LIGHT) {
        if (is_domain(entity, "light")) return true;
        ESP_LOGW(TAG, "layout: %s is not a light, skipped", entity);
        return false;
    }
    if (kind == LAYOUT_MEDIA) {
        if (is_domain(entity, "media_player")) return true;
        ESP_LOGW(TAG, "layout: %s is not a media player, skipped", entity);
        return false;
    }
    if (kind == LAYOUT_THERMOSTAT) {
        if (is_domain(entity, "climate")) return true;
        ESP_LOGW(TAG, "layout: %s is not a climate entity, skipped", entity);
        return false;
    }
    if (kind == LAYOUT_TODO) {
        if (is_domain(entity, "todo")) return true;
        ESP_LOGW(TAG, "layout: %s is not a todo list, skipped", entity);
        return false;
    }
    if (kind == LAYOUT_CALENDAR) {
        if (is_domain(entity, "calendar")) return true;
        ESP_LOGW(TAG, "layout: %s is not a calendar, skipped", entity);
        return false;
    }
    if (kind == LAYOUT_PICTURE) {
        if (is_domain(entity, "camera") || is_domain(entity, "image")) return true;
        ESP_LOGW(TAG, "layout: %s is not a camera or image, no picture", entity);
        return false;
    }
    char *end;
    strtof(state, &end);
    if (end != state) return true;
    ESP_LOGW(TAG, "layout: %s is not numeric ('%s'), no graph", entity, state);
    return false;
}

/* The number after `key` as HA wrote it (e.g. "4.5"); "" if missing */
static void json_numtext(const char *json, const char *key, char *out, size_t n)
{
    out[0] = 0;
    const char *p = strstr(json, key);
    if (!p) return;
    p += strlen(key);
    size_t k = 0;
    while (k + 1 < n && (p[k] == '-' || p[k] == '.' || (p[k] >= '0' && p[k] <= '9'))) { out[k] = p[k]; k++; }
    out[k] = 0;
}

/* Weather card: condition, temperature and humidity from the entity's attributes */
static void fetch_weather(int pg, int row)
{
    resp_t *r = &s_rbuf;   /* shared: all callers run on the HA task, one at a time */
    const layout_item_t *it = &s_layout.pages[pg].items[row];
    char path[96];
    snprintf(path, sizeof(path), "/api/states/%s", it->entity);
    if (ha_request(HTTP_METHOD_GET, path, NULL, r) != 200) return;   /* keep the last data */
    parse_weather(pg, row, r->buf);
    if (it->flags & LAYOUT_WX_FORECAST) fetch_forecast(pg, row);
}

static void parse_weather(int pg, int row, const char *json)
{
    const layout_item_t *it = &s_layout.pages[pg].items[row];
    char cond[24] = "", temp[12] = "", hum[8] = "", unit[8] = "", name[32];
    snprintf(name, sizeof(name), "%.31s", it->name[0] ? it->name : it->entity);
    if (!it->name[0] && json_str(json, "\"friendly_name\":\"", name, sizeof(name))) utf8_trim(name);
    json_str(json, "\"state\":\"", cond, sizeof(cond));
    if (!strcmp(cond, "unavailable") || !strcmp(cond, "unknown")) cond[0] = 0;
    json_numtext(json, "\"temperature\":", temp, sizeof(temp));
    json_numtext(json, "\"humidity\":", hum, sizeof(hum));
    json_str(json, "\"temperature_unit\":\"", unit, sizeof(unit));
    emit_weather(pg, row, cond, temp, unit, hum, name);
}

/* Thermostat card: mode, action and temperatures of a climate entity */
static void parse_climate(int pg, int row, const char *json)
{
    const layout_item_t *it = &s_layout.pages[pg].items[row];
    ha_climate_t c = {0};
    char name[32];
    snprintf(name, sizeof(name), "%.31s", it->name[0] ? it->name : it->entity);
    report_icon(pg, row, json);
    if (!it->name[0] && json_str(json, "\"friendly_name\":\"", name, sizeof(name))) utf8_trim(name);
    json_str(json, "\"state\":\"", c.mode, sizeof(c.mode));
    json_str(json, "\"hvac_action\":\"", c.action, sizeof(c.action));
    json_numtext(json, "\"current_temperature\":", c.cur, sizeof(c.cur));
    json_numtext(json, "\"temperature\":", c.target, sizeof(c.target));   /* not current_: '"' first */
    json_numtext(json, "\"min_temp\":", c.min, sizeof(c.min));
    json_numtext(json, "\"max_temp\":", c.max, sizeof(c.max));
    json_numtext(json, "\"target_temp_step\":", c.step, sizeof(c.step));
    emit_climate(pg, row, &c, name);
}

static void fetch_climate(int pg, int row)
{
    resp_t *r = &s_rbuf;   /* shared: all callers run on the HA task, one at a time */
    char path[96];
    snprintf(path, sizeof(path), "/api/states/%s", s_layout.pages[pg].items[row].entity);
    if (ha_request(HTTP_METHOD_GET, path, NULL, r) != 200) return;   /* keep the last data */
    if (r->len >= (int)sizeof(r->buf) - 1) ESP_LOGW(TAG, "%s: state longer than %d bytes, cut", s_layout.pages[pg].items[row].entity, (int)sizeof(r->buf) - 1);
    parse_climate(pg, row, r->buf);
}

/* Media control card: state, title and artist of a media player */
static void parse_media(int pg, int row, const char *json)
{
    const layout_item_t *it = &s_layout.pages[pg].items[row];
    char st[16] = "", title[40] = "", artist[32] = "", name[32];
    snprintf(name, sizeof(name), "%.31s", it->name[0] ? it->name : it->entity);
    if (!it->name[0] && json_str(json, "\"friendly_name\":\"", name, sizeof(name))) utf8_trim(name);
    json_str(json, "\"state\":\"", st, sizeof(st));
    if (json_str(json, "\"media_title\":\"", title, sizeof(title))) utf8_trim(title);
    if (json_str(json, "\"media_artist\":\"", artist, sizeof(artist))) utf8_trim(artist);
    emit_media(pg, row, st, title, artist, name);
}

static void fetch_media(int pg, int row)
{
    resp_t *r = &s_rbuf;   /* shared: all callers run on the HA task, one at a time */
    char path[96];
    snprintf(path, sizeof(path), "/api/states/%s", s_layout.pages[pg].items[row].entity);
    if (ha_request(HTTP_METHOD_GET, path, NULL, r) != 200) return;   /* keep the last data */
    if (r->len >= (int)sizeof(r->buf) - 1) ESP_LOGW(TAG, "%s: state longer than %d bytes, cut", s_layout.pages[pg].items[row].entity, (int)sizeof(r->buf) - 1);
    parse_media(pg, row, r->buf);
}

/* Light card: on/off and brightness (0-255 in HA, shown in %) */
static void parse_light(int pg, int row, const char *json)
{
    const layout_item_t *it = &s_layout.pages[pg].items[row];
    char st[16] = "", name[32];
    float bri;
    snprintf(name, sizeof(name), "%.31s", it->name[0] ? it->name : it->entity);
    if (!it->name[0] && json_str(json, "\"friendly_name\":\"", name, sizeof(name))) utf8_trim(name);
    json_str(json, "\"state\":\"", st, sizeof(st));
    int pct = -1;
    if (json_num(json, "\"brightness\":", &bri) && bri >= 0) pct = (int)(bri * 100 / 255 + 0.5f);
    if (pct > 100) pct = 100;
    emit_light(pg, row, st, pct, name);
}

static void fetch_light(int pg, int row)
{
    resp_t *r = &s_rbuf;   /* shared: all callers run on the HA task, one at a time */
    char path[96];
    snprintf(path, sizeof(path), "/api/states/%s", s_layout.pages[pg].items[row].entity);
    if (ha_request(HTTP_METHOD_GET, path, NULL, r) != 200) return;   /* keep the last data */
    parse_light(pg, row, r->buf);
}

/* Forecast of a weather card through weather.get_forecasts, on every poll like the rest */
static void fc_entry(char *a)
{
    if (s_fc_n >= HA_FORECAST_MAX) return;
    ha_forecast_t *x = &s_fc[s_fc_n];
    memset(x, 0, sizeof(*x));
    char dt[32] = "";
    json_str(a, "\"datetime\":\"", dt, sizeof(dt));
    json_str(a, "\"condition\":\"", x->cond, sizeof(x->cond));
    json_numtext(a, "\"temperature\":", x->hi, sizeof(x->hi));
    json_numtext(a, "\"templow\":", x->lo, sizeof(x->lo));
    x->t = parse_iso(dt);
    const char *z = strlen(dt) > 19 ? strpbrk(dt + 19, "+-") : NULL;   /* "+01:00": local time */
    int oh, om;
    if (x->t && z && sscanf(z + 1, "%d:%d", &oh, &om) == 2) x->t -= (z[0] == '-' ? -1 : 1) * (oh * 3600 + om * 60);
    if (x->t) s_fc_n++;
}

static void fetch_forecast(int pg, int row)
{
    const layout_item_t *it = &s_layout.pages[pg].items[row];
    bool hourly = it->flags & LAYOUT_WX_HOURLY;
    char body[128];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",\"type\":\"%s\"}", it->entity, hourly ? "hourly" : "daily");
    s_fc_mode = true;
    int st = ha_request(HTTP_METHOD_POST, "/api/services/weather/get_forecasts?return_response", body, NULL);
    s_fc_mode = false;
    if (st != 200) return;               /* keep the last data */
    emit_forecast(pg, row, s_fc, s_fc_n, hourly);
}

/* Calendar card: the events of all its calendars through calendar.get_events, on every poll
 * like the rest: day view = today, list view = now + 7 days, month view = this month */
/* Calendar colours as HA's calendar card picks them: the calendar's colour from the entity
 * registry (options.calendar.color), else HA's palette (--color-N) by its place in the card.
 * Read once per layout and card. */
#define CAL_IDS_MAX 8
static uint32_t s_cal_rgb[LAYOUT_MAX_CALS][CAL_IDS_MAX];
static bool s_cal_rgb_ok[LAYOUT_MAX_CALS];
static const uint32_t CAL_PALETTE[CAL_IDS_MAX] = {
    0x4269d0, 0xf4bd4a, 0xff725c, 0x6cc5b0, 0xa463f2, 0xff8ab7, 0x9c6b4e, 0x97bbf5,
};

static void cal_colors(int k)
{
    const char *ids = s_layout.cals[k];
    for (int i = 0; i < CAL_IDS_MAX; i++) s_cal_rgb[k][i] = CAL_PALETTE[i];
    dash_ws_t *d = dash_ws_open();
    if (!d) return;                       /* palette for now; tried again on the next poll */
    bool ok = true;
    int i = 0;
    for (const char *p = ids; *p && i < CAL_IDS_MAX; i++) {
        size_t m = strcspn(p, ",");
        char body[128];
        snprintf(body, sizeof(body), "\"type\":\"config/entity_registry/get\",\"entity_id\":\"%.*s\"", (int)m, p);
        p += m + (p[m] == ',');
        char *msg = dash_ws_call(d, body);
        if (!msg) { ok = false; break; }
        cJSON *root = cJSON_Parse(msg);
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(
                         cJSON_GetObjectItemCaseSensitive(root, "result"), "options"), "calendar"), "color");
        unsigned v;
        if (cJSON_IsString(c) && c->valuestring[0] == '#' && strlen(c->valuestring) == 7 && sscanf(c->valuestring + 1, "%6x", &v) == 1)
            s_cal_rgb[k][i] = v;
        else if (cJSON_IsString(c) && c->valuestring[0])
            ESP_LOGW(TAG, "calendar colour '%s' not supported, palette used", c->valuestring);
        cJSON_Delete(root);
        free(msg);
    }
    dash_ws_close(d);
    s_cal_rgb_ok[k] = ok;
}

static void fetch_calendar(int pg, int row)
{
    const layout_item_t *it = &s_layout.pages[pg].items[row];
    if (!it->cal || it->cal > LAYOUT_MAX_CALS) return;
    int view = it->flags & LAYOUT_CAL_SPAN;
    const char *ids = s_layout.cals[it->cal - 1];
    char body[LAYOUT_CALS_LEN * 2 + 160];
    int n = snprintf(body, sizeof(body), "{\"entity_id\":[");
    for (const char *p = ids; *p && n < (int)sizeof(body) - 120; ) {
        size_t k = strcspn(p, ",");
        n += snprintf(body + n, sizeof(body) - n, "%s\"%.*s\"", p == ids ? "" : ",", (int)k, p);
        p += k + (p[k] == ',');
    }
    time_t now = time(NULL), from = now, to = now + 7 * 86400;
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0; tm.tm_isdst = -1;
    if (view == LAYOUT_CAL_DAY) {          /* local midnight to midnight */
        from = mktime(&tm);
        tm.tm_mday++; tm.tm_isdst = -1;
        to = mktime(&tm);
    } else if (view == LAYOUT_CAL_MONTH) { /* the 1st to the 1st of the next month */
        tm.tm_mday = 1;
        from = mktime(&tm);
        tm.tm_mon++; tm.tm_isdst = -1;
        to = mktime(&tm);
    }
    struct tm u;
    gmtime_r(&from, &u);
    snprintf(body + n, sizeof(body) - n, "],\"start_date_time\":\"%04d-%02d-%02dT%02d:%02d:%02d+00:00\",\"duration\":{\"seconds\":%ld}}",
             u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec, (long)(to - from));
    static cal_parse_t cp;
    cp.mask_from = view == LAYOUT_CAL_MONTH ? from : 0;
    cp.ids = ids;
    if (!s_cal_rgb_ok[it->cal - 1]) cal_colors(it->cal - 1);
    cal_parse_init(&cp);
    s_cal = &cp;
    int st = ha_request(HTTP_METHOD_POST, "/api/services/calendar/get_events?return_response", body, NULL);
    s_cal = NULL;
    if (st != 200) return;               /* keep the last data */
    for (int k = 0; k < cp.n; k++) cp.ev[k].color = s_cal_rgb[it->cal - 1][cp.ev[k].src % CAL_IDS_MAX];
    emit_calendar(pg, row, cp.ev, cp.n, cp.mask, cp.mask_from);
}

/* Drop a UTF-8 sequence cut off at the end of s by truncation */
static void utf8_trim(char *s)
{
    int n = strlen(s), i = n;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;   /* continuation bytes */
    if (i == 0) return;
    unsigned char lead = s[i - 1];
    int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - (i - 1) < need) s[i - 1] = 0;
}

/* Card names given as types (floor, device, entity, ...): rendered once per dashboard
 * change with HA's template API. Empty or failed results fall back to friendly_name. */
static void resolve_names(ha_layout_t *l)
{
    resp_t *r = &s_rbuf;   /* shared: all callers run on the HA task, one at a time */
    static char tpl[1024], body[1200];
    for (int p = 0; p < l->count; p++) {
        for (int i = 0; i < l->pages[p].count; i++) {
            layout_item_t *it = &l->pages[p].items[i];
            if (it->name[0] || !layout_name_template(it, tpl, sizeof(tpl))) continue;
            int n = snprintf(body, sizeof(body), "{\"template\":\"");
            for (const char *c = tpl; *c && n < (int)sizeof(body) - 4; c++) {
                if (*c == '"' || *c == '\\') body[n++] = '\\';
                body[n++] = *c;
            }
            snprintf(body + n, sizeof(body) - n, "\"}");
            if (ha_request(HTTP_METHOD_POST, "/api/template", body, r) != 200) continue;
            snprintf(it->name, sizeof(it->name), "%.31s", r->buf);
            utf8_trim(it->name);
            ESP_LOGI(TAG, "name %s -> '%s'", it->entity, it->name);
        }
    }
}

/* Icons: the card's icon, else the entity's icon attribute, else HA's icon tables (the
 * integration's icon for the entity's translation key, else the domain's icon for its device
 * class, else the domain default), like HA's frontend */
static bool has_icon(const layout_item_t *it)
{
    return (it->kind == LAYOUT_VALUES || it->kind == LAYOUT_GRAPH || it->kind == LAYOUT_BUTTONS || it->kind == LAYOUT_GLANCE ||
            it->kind == LAYOUT_ENTITIES) &&
           it->icon[0] != '-';
}

static bool s_icon_rules_pending;      /* the last load failed (no memory/connection): retry */
static void load_icon_rules(void)
{
    s_icon_rules_pending = false;
    for (int p = 0; p < LAYOUT_MAX_PAGES; p++)
        for (int i = 0; i < LAYOUT_MAX_ROWS; i++) { free(s_icon_rule[p][i]); s_icon_rule[p][i] = NULL; }
    memset(s_icon_last, 0, sizeof(s_icon_last));
    dash_ws_t *ws = NULL;
    for (int p = 0; p < s_layout.count; p++)
        for (int i = 0; i < s_layout.pages[p].count; i++) {
            const layout_item_t *it = &s_layout.pages[p].items[i];
            if (!has_icon(it) || it->icon[0]) continue;
            if (!ws) {
                if (!(xEventGroupGetBits(s_events) & WIFI_UP_BIT)) { s_icon_rules_pending = true; return; }
                if (s_http) {             /* two TLS sessions at once don't fit in RAM */
                    esp_http_client_cleanup(s_http);
                    s_http = NULL;
                }
                if (!(ws = dash_ws_open())) { s_icon_rules_pending = true; return; }
            }
            char body[200], platform[32] = "", tkey[48] = "", dc[32] = "", domain[24], rule[320] = "";
            snprintf(domain, sizeof(domain), "%.*s", (int)strcspn(it->entity, "."), it->entity);
            snprintf(body, sizeof(body), "\"type\":\"config/entity_registry/get\",\"entity_id\":\"%s\"", it->entity);
            char *msg = dash_ws_call(ws, body);
            const char *reg = json_obj_get(msg, "result");
            json_get_str(json_obj_get(reg, "platform"), platform, sizeof(platform));
            json_get_str(json_obj_get(reg, "translation_key"), tkey, sizeof(tkey));
            if (!json_get_str(json_obj_get(reg, "device_class"), dc, sizeof(dc)))
                json_get_str(json_obj_get(reg, "original_device_class"), dc, sizeof(dc));
            free(msg);
            /* not in the registry: no device class here (an HTTPS request now would need a
             * second TLS session next to the websocket, which doesn't fit in RAM); the
             * domain default applies */
            if (platform[0] && tkey[0]) {
                snprintf(body, sizeof(body), "\"type\":\"frontend/get_icons\",\"category\":\"entity\",\"integration\":\"%s\"", platform);
                msg = dash_ws_call(ws, body);
                icon_rule_from_icons(msg, platform, domain, tkey, rule, sizeof(rule));
                free(msg);
            }
            if (!rule[0]) {
                snprintf(body, sizeof(body), "\"type\":\"frontend/get_icons\",\"category\":\"entity_component\",\"integration\":\"%s\"", domain);
                msg = dash_ws_call(ws, body);
                if (!dc[0] || !icon_rule_from_icons(msg, domain, NULL, dc, rule, sizeof(rule)))
                    icon_rule_from_icons(msg, domain, NULL, "_", rule, sizeof(rule));
                free(msg);
            }
            ESP_LOGI(TAG, "icon %s: %s", it->entity, rule[0] ? rule : "(none)");
            s_icon_rule[p][i] = rule[0] ? strdup(rule) : NULL;
        }
    dash_ws_close(ws);
}

/* The icon for this state (json = /api/states result), reported when it changes */
static void report_icon(int pg, int row, const char *json)
{
    const layout_item_t *it = &s_layout.pages[pg].items[row];
    char icon[48] = "", state[48] = "";
    if (!has_icon(it)) return;
    if (it->icon[0]) snprintf(icon, sizeof(icon), "%s", it->icon);
    else if (!json_str(json, "\"icon\":\"", icon, sizeof(icon))) {
        json_str(json, "\"state\":\"", state, sizeof(state));
        icon_rule_apply(s_icon_rule[pg][row], state, icon, sizeof(icon));
    }
    if (!strcmp(icon, s_icon_last[pg][row])) return;
    strcpy(s_icon_last[pg][row], icon);
    emit_icon(pg, row, icon);
}

/* ---- Conditions (issue #57): which of the layout's conditions hold now. s_layout only has
 * the cards and pages shown; the whole layout stays in the flash cache. HA task only. */
static layout_mask_t s_cond_true;
static bool s_cond_dirty;          /* s_cond_true changed: show the layout again */
static uint32_t s_full_hash;       /* the whole layout last saved to the cache */

/* The mask as last seen, in NVS: a boot from the cache shows the cards that were shown (and the
 * cached data, tagged with that layout's hash, still fits them) */
static void cond_save(void)
{
    static layout_mask_t saved;
    static bool known;
    if (known && saved == s_cond_true) return;
    nvs_handle_t h;
    if (nvs_open("board", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "cond", s_cond_true);
    nvs_commit(h);
    nvs_close(h);
    saved = s_cond_true;
    known = true;
}

static layout_mask_t cond_load(void)
{
    nvs_handle_t h;
    uint16_t v = 0;
    if (nvs_open("board", NVS_READONLY, &h) == ESP_OK) { nvs_get_u16(h, "cond", &v); nvs_close(h); }
    return v;
}

static layout_mask_t cond_bit(const ha_layout_t *l, const char *entity, const char *state)
{
    layout_mask_t m = s_cond_true;
    for (int k = 0; k < LAYOUT_MAX_CONDS; k++) {
        if (!l->conds[k].entity[0] || strcmp(l->conds[k].entity, entity)) continue;
        if (layout_cond_true(&l->conds[k], state)) m |= (layout_mask_t)(1u << k);
        else m &= (layout_mask_t)~(1u << k);
    }
    return m;
}

/* Every condition of l, read over HTTP (a 404 / no answer: the state is not known) */
static layout_mask_t cond_seed(const ha_layout_t *l)
{
    resp_t *r = &s_rbuf;   /* shared: all callers run on the HA task, one at a time */
    char path[96], state[LAYOUT_COND_LEN];
    s_cond_true = 0;
    for (int k = 0; k < LAYOUT_MAX_CONDS; k++) {
        const char *e = l->conds[k].entity;
        bool seen = false;
        for (int j = 0; j < k && !seen; j++) seen = !strcmp(l->conds[j].entity, e);
        if (!e[0] || seen) continue;
        snprintf(path, sizeof(path), "/api/states/%s", e);
        r->len = 0;
        bool ok = ha_request(HTTP_METHOD_GET, path, NULL, r) == 200 && json_str(r->buf, "\"state\":\"", state, sizeof(state));
        s_cond_true = cond_bit(l, e, ok ? state : NULL);
    }
    return s_cond_true;
}

static void use_layout(const ha_layout_t *l);
static ha_layout_t s_scratch;

/* The cached whole layout with the cards and pages whose conditions hold now */
static void cond_apply(void)
{
    s_cond_dirty = false;
    if (!layout_store_load(&s_scratch)) return;
    cond_save();
    layout_visible(&s_scratch, s_cond_true);
    if (layout_hash(&s_scratch) == s_layout_hash) return;
    ESP_LOGI(TAG, "conditions changed: %d pages shown", s_scratch.count);
    use_layout(&s_scratch);
}

static void use_layout(const ha_layout_t *l)
{
    rec_lock();
    if (layout_hash(l) != s_layout_hash) rec_clear();   /* the rows mean something else now */
    s_layout = *l;
    s_layout_hash = layout_hash(l);
    memset(s_cal_rgb_ok, 0, sizeof(s_cal_rgb_ok));   /* calendar colours: read again */
    rec_unlock();
    if (s_hold) s_held_layout = true;
    else if (s_layout_cb) s_layout_cb(&s_layout);
    load_icon_rules();
}

/* The theme's colours from frontend/get_themes; kept as they are when HA doesn't answer */
static void fetch_theme(const char *name)
{
    if (!s_theme_cb) return;
    if (s_http) {                         /* two TLS sessions at once don't fit in RAM */
        esp_http_client_cleanup(s_http);
        s_http = NULL;
    }
    dash_ws_t *d = dash_ws_open();
    char *msg = d ? dash_ws_call(d, "\"type\":\"frontend/get_themes\"") : NULL;
    dash_ws_close(d);
    if (!msg) return;
    static ha_theme_t t;
    if (!theme_parse(msg, name, &t) && name[0]) ESP_LOGW(TAG, "theme '%s' not found or without colours", name);
    free(msg);
    s_theme_cb(&t);
}

/* Re-read the dashboard; switch to it if it is usable and differs from the current one.
 * Failures keep the current layout and never reach the error box. */
/* force: re-check entities and names even if the dashboard itself is unchanged */
/* one scratch layout for the boot cache and every dashboard re-read (~12 KB of RAM) */

static void fetch_todo(int pg, int row);
/* End of a switch round: the new layout (if any) with its records to the UI, which replays
 * them; todo lists aren't recorded, so they are read again */
static void hold_release(void)
{
    if (!s_hold) return;
    s_hold = false;
    if (!s_held_layout) { s_held_theme = false; return; }
    s_held_layout = false;
    ESP_LOGI(TAG, "switch: new layout to the UI");
    if (s_held_theme) fetch_theme(s_layout.theme);
    s_held_theme = false;
    if (s_layout_cb) s_layout_cb(&s_layout);
    for (int pg = 0; pg < s_layout.count; pg++)
        for (int i = 0; i < s_layout.pages[pg].count; i++)
            if (s_layout.pages[pg].items[i].kind == LAYOUT_TODO) fetch_todo(pg, i);
}

static void refresh_layout(bool force)
{
    ha_layout_t *lp = &s_scratch;
#define l (*lp)
    static uint32_t raw_hash;             /* parsed but unfiltered: skip the entity checks when unchanged */
    if (s_http) {                         /* two TLS sessions at once don't fit in RAM */
        esp_http_client_cleanup(s_http);
        s_http = NULL;
    }
    static layout_view_t views[LAYOUT_MAX_VIEWS];
    char want[32], chosen[32], dash[32];
    int nv;
    taskENTER_CRITICAL(&s_view_lock);
    memcpy(want, s_view, sizeof(want));
    memcpy(dash, s_dash, sizeof(dash));
    taskEXIT_CRITICAL(&s_view_lock);
    char *list = NULL;
    char *json = dash_fetch_config(dash, s_hold ? NULL : &list);   /* a switch: the UI has the list */
    if (list) report_dashboards(list, dash);
    free(list);
    if (!json) return;
    int n = layout_parse_views(json, want, &l, views, LAYOUT_MAX_VIEWS, &nv, chosen);
    free(json);
    if (s_views_cb) {
        int cur = -1;
        for (int i = 0; i < nv; i++) if (!strcmp(views[i].path, chosen)) cur = i;
        s_views_cb(views, nv, cur);
    }
    if (!force && n && layout_hash(&l) == raw_hash && s_layout.count) return;
    uint32_t h = layout_hash(&l);
    if (n) n = layout_filter(&l, entity_usable, NULL);
    if (n) resolve_names(&l);
    if (n) raw_hash = h;
    s_err[0] = 0;                         /* validation errors are not shown */
    if (!n) { ESP_LOGW(TAG, "dashboard has no usable pages, keeping the current layout"); return; }
    if (s_hold) s_held_theme = true;      /* with the layout, on release: a theme rebuild now would empty the old view */
    else fetch_theme(l.theme);            /* the view's theme: as often as the dashboard is read anew */
    uint32_t full = layout_hash(&l);
    if (full != s_full_hash) {            /* the whole layout: shown cards come from it */
        layout_store_save(&l);
        s_full_hash = full;
    }
    cond_seed(&l);
    s_cond_dirty = false;
    cond_save();
    n = layout_visible(&l, s_cond_true);
    if (layout_hash(&l) == s_layout_hash) return;
    ESP_LOGI(TAG, "new layout: %d pages shown", n);
    use_layout(&l);
#undef l
}

#define PIC_MAX1(x) ((x) > 1 ? (x) : 1)
/* ---- Picture cards: JPEG or PNG, decoded while it streams in (never held whole in RAM) ---- */
typedef struct {
    esp_http_client_handle_t http;
    uint8_t head[8];                      /* bytes read to sniff the format, handed out first */
    int head_len, head_pos;
    uint16_t *px;                         /* output, ow x oh */
    int ow, oh;                           /* output size */
    int vw, vh, cx, cy;                   /* image scaled to vw x vh, cropped from (cx, cy) */
    uint8_t fit;                          /* LAYOUT_PIC_COVER / _FILL, else contain */
    uint8_t zoom;                         /* 2: decoded at half size, shown zoomed (low memory) */
    int iw, ih, bw, bh;                   /* image and block size, for a refit at half size */
    int sw, sh;                           /* size of the decoded image (JPEG: 1/2^scale) */
    uint8_t *win;                         /* PNG inflate window / JPEG work area (32 KB) */
    tinfl_decompressor *inf;              /* PNG inflate state */
} pic_dec_t;

/* Next n bytes of the body (buf NULL = skip them); returns how many were read */
static int pic_read(pic_dec_t *d, uint8_t *buf, int n)
{
    static uint8_t skip[256];
    int got = 0;
    while (got < n && d->head_pos < d->head_len) {
        if (buf) buf[got] = d->head[d->head_pos];
        got++; d->head_pos++;
    }
    while (got < n) {
        int want = n - got;
        uint8_t *dst = buf ? buf + got : skip;
        if (!buf && want > (int)sizeof(skip)) want = sizeof(skip);
        int r = esp_http_client_read(d->http, (char *)dst, want);
        if (r <= 0) break;
        got += r;
    }
    return got;
}

/* Scale iw x ih for the block bw x bh: contain keeps the whole image (aspect kept), cover
 * fills the block and crops the overflow evenly, fill stretches */
static void pic_fit(pic_dec_t *d, int iw, int ih, int bw, int bh)
{
    d->iw = iw; d->ih = ih; d->bw = bw; d->bh = bh;
    bool wide = (int64_t)iw * bh > (int64_t)ih * bw;
    if (d->fit & LAYOUT_PIC_FILL) { d->vw = bw; d->vh = bh; }
    else if (wide != !!(d->fit & LAYOUT_PIC_COVER)) { d->vw = bw; d->vh = PIC_MAX1((int)((int64_t)ih * bw / iw)); }
    else { d->vh = bh; d->vw = PIC_MAX1((int)((int64_t)iw * bh / ih)); }
    d->ow = d->vw < bw ? d->vw : bw;
    d->oh = d->vh < bh ? d->vh : bh;
    d->cx = (d->vw - d->ow) / 2;
    d->cy = (d->vh - d->oh) / 2;
}

/* Output range [*a, *b) covered by source pixel i of n (scaled to v, cropped from c, size o) */
static bool pic_span(int i, int n, int v, int c, int o, int *a, int *b)
{
    *a = (int)((int64_t)i * v / n) - c;
    *b = (int)((int64_t)(i + 1) * v / n) - c;
    if (*b <= *a) *b = *a + 1;
    if (*a < 0) *a = 0;
    if (*b > o) *b = o;
    return *a < *b;
}

static inline uint16_t rgb565s(int r, int g, int b)   /* LV_COLOR_16_SWAP byte order */
{
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (uint16_t)((c >> 8) | (c << 8));
}

static UINT jpg_in(JDEC *jd, BYTE *buf, UINT n) { return pic_read(jd->device, buf, n); }

/* One decoded block (RGB888) -> nearest pixels of the fitted output */
static UINT jpg_out(JDEC *jd, void *bitmap, JRECT *rc)
{
    pic_dec_t *d = jd->device;
    const uint8_t *p = bitmap;
    int w = rc->right - rc->left + 1;
    for (int y = rc->top; y <= rc->bottom; y++, p += w * 3) {
        int y0, y1;
        if (!pic_span(y, d->sh, d->vh, d->cy, d->oh, &y0, &y1)) continue;
        const uint8_t *q = p;
        for (int x = rc->left; x <= rc->right; x++, q += 3) {
            int x0, x1;
            if (!pic_span(x, d->sw, d->vw, d->cx, d->ow, &x0, &x1)) continue;
            uint16_t c = rgb565s(q[0], q[1], q[2]);
            for (int oy = y0; oy < y1; oy++)
                for (int ox = x0; ox < x1; ox++) d->px[oy * d->ow + ox] = c;
        }
    }
    return 1;
}

/* Reserves the output for the whole block before the connection opens (TLS and the PNG
 * window split the heap up); when short on memory the old picture is dropped first. */
/* Decoder buffers are static: 43 KB that a fragmented heap rarely has in one piece */


/* Output at the fitted size, allocated once the header is known; when short on memory the
 * old picture is dropped first */
static bool pic_alloc(pic_dec_t *d, int pg, int row)
{
    size_t sz = (size_t)d->ow * d->oh * 2;
    d->px = malloc(sz);
    if (!d->px && s_pic_cb) {
        s_pic_cb(pg, row, NULL, 0, 0, 1, NULL);
        d->px = malloc(sz);
    }
    if (!d->px) {                         /* still too big: half size, zoomed back up by LVGL */
        int ow = d->ow, oh = d->oh;
        pic_fit(d, d->iw, d->ih, PIC_MAX1(d->bw / 2), PIC_MAX1(d->bh / 2));
        d->px = malloc((size_t)d->ow * d->oh * 2);
        if (d->px) { d->zoom = 2; sz = (size_t)d->ow * d->oh * 2; }
        else { d->ow = ow; d->oh = oh; }
    }
    if (!d->px) {
        ESP_LOGW(TAG, "picture %dx%d: no memory (free %u, largest %u)", d->ow, d->oh, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return false;
    }
    memset(d->px, 0, sz);
    return true;
}

static const char *jpg_decode(pic_dec_t *d, int bw, int bh, int pg, int row)
{
    void *work = d->win;                  /* 32 KB reserved up front; JPEG needs 3100 bytes */
    JDEC jd;
    const char *err = NULL;
    JRESULT r = jd_prepare(&jd, jpg_in, work, 3100, d);
    if (r != JDR_OK || !jd.width || !jd.height) err = "JPEG header error";
    if (!err) {
        pic_fit(d, jd.width, jd.height, bw, bh);
        int scale = 0;                    /* largest 1/2^n that still has enough pixels */
        while (scale < 3 && (int)(jd.width >> (scale + 1)) >= d->vw && (int)(jd.height >> (scale + 1)) >= d->vh) scale++;
        d->sw = PIC_MAX1((int)((jd.width + (1 << scale) - 1) >> scale));
        d->sh = PIC_MAX1((int)((jd.height + (1 << scale) - 1) >> scale));
        if (!pic_alloc(d, pg, row)) err = "no memory";
        else if (jd_decomp(&jd, jpg_out, scale) != JDR_OK) err = "JPEG decode error (progressive?)";
    }
    return err;
}

/* PNG: chunks are walked as they arrive; IDAT data goes through the ROM's inflate into a
 * 32 KB window, rows are unfiltered one at a time and scaled straight into the output. */
typedef struct {
    pic_dec_t *d;
    uint32_t left;                        /* bytes left in the current IDAT chunk */
    bool end;                             /* no more IDAT */
} png_in_t;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* Next compressed bytes from the IDAT chunks; 0 at the end */
static int png_idat(png_in_t *in, uint8_t *buf, int n)
{
    while (!in->left && !in->end) {
        uint8_t h[12];                    /* CRC of the previous chunk + next length and type */
        if (pic_read(in->d, h, 12) != 12 || memcmp(h + 8, "IDAT", 4)) { in->end = true; break; }
        in->left = be32(h + 4);
    }
    if (in->end) return 0;
    if ((uint32_t)n > in->left) n = in->left;
    int r = pic_read(in->d, buf, n);
    if (r <= 0) { in->end = true; return 0; }
    in->left -= r;
    return r;
}

static inline int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static const char *png_decode(pic_dec_t *d, int bw, int bh, int pg, int row)
{
    uint8_t h[8 + 8 + 13];                /* signature, IHDR header and data */
    if (pic_read(d, h, sizeof(h)) != sizeof(h) || memcmp(h + 12, "IHDR", 4)) return "PNG header error";
    int W = be32(h + 16), H = be32(h + 20), depth = h[24], ct = h[25];
    if (h[28]) return "interlaced PNG not supported";
    static const uint8_t chans[7] = { 1, 0, 3, 1, 2, 0, 4 };
    if (W <= 0 || H <= 0 || ct > 6 || !chans[ct] || (depth != 8 && !(ct == 3 && depth < 8) && !(ct == 0 && depth < 8)))
        return "PNG format not supported (needs 8-bit, or palette/grey)";
    int bpp = PIC_MAX1(chans[ct] * depth / 8);           /* bytes per pixel for the filters */
    int rowb = (W * chans[ct] * depth + 7) / 8;
    pic_read(d, NULL, 4);                 /* IHDR CRC */

    /* chunks before the image data: keep the palette, skip the rest */
    static uint8_t pal[256 * 3];
    uint32_t first = 0;
    for (;;) {
        uint8_t c[8];
        if (pic_read(d, c, 8) != 8) return "PNG ended early";
        uint32_t len = be32(c);
        if (!memcmp(c + 4, "IDAT", 4)) { first = len; break; }
        if (!memcmp(c + 4, "PLTE", 4) && len <= sizeof(pal)) pic_read(d, pal, len);
        else pic_read(d, NULL, len);
        pic_read(d, NULL, 4);             /* CRC */
    }

    pic_fit(d, W, H, bw, bh);
    d->sw = W; d->sh = H;
    const char *err = NULL;
    tinfl_decompressor *inf = d->inf;
    uint8_t *win = d->win, *inb = malloc(1024);
    uint8_t *cur = malloc(rowb + 1), *prev = calloc(1, rowb + 1);
    if (!inb || !cur || !prev || !pic_alloc(d, pg, row)) err = "no memory";
    if (!err) {
        tinfl_init(inf);
        png_in_t in = { .d = d, .left = first };
        size_t in_len = 0, in_pos = 0, win_pos = 0;
        int fill = 0, y = 0;              /* bytes in cur (filter byte first), current row */
        tinfl_status st = TINFL_STATUS_NEEDS_MORE_INPUT;
        while (y < H && !err) {
            if (in_pos == in_len && st == TINFL_STATUS_NEEDS_MORE_INPUT) {
                in_len = png_idat(&in, inb, 1024);
                in_pos = 0;
                if (!in_len) { err = "PNG data ended early"; break; }
            }
            size_t n_in = in_len - in_pos, n_out = TINFL_LZ_DICT_SIZE - win_pos;
            st = tinfl_decompress(inf, inb + in_pos, &n_in, win, win + win_pos, &n_out,
                                  TINFL_FLAG_PARSE_ZLIB_HEADER | (in.end ? 0 : TINFL_FLAG_HAS_MORE_INPUT));
            in_pos += n_in;
            if (st < 0) { err = "PNG data error"; break; }
            for (size_t k = 0; k < n_out && y < H; k++) {
                cur[fill++] = win[win_pos + k];
                if (fill < rowb + 1) continue;
                /* a full row: undo its filter, then scale it into the output */
                uint8_t f = cur[0], *r = cur + 1, *p = prev + 1;
                for (int i = 0; i < rowb; i++) {
                    int a = i >= bpp ? r[i - bpp] : 0, b = p[i], c = i >= bpp ? p[i - bpp] : 0;
                    r[i] += f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : f == 4 ? paeth(a, b, c) : 0;
                }
                int y0, y1;
                uint16_t *o = NULL;
                if (pic_span(y, H, d->vh, d->cy, d->oh, &y0, &y1)) o = d->px + y0 * d->ow;
                for (int ox = 0; o && ox < d->ow; ox++) {
                    int x = (int)((int64_t)(ox + d->cx) * W / d->vw), R, G, B, A = 255;
                    if (ct == 2 || ct == 6) { const uint8_t *q = r + x * chans[ct]; R = q[0]; G = q[1]; B = q[2]; if (ct == 6) A = q[3]; }
                    else if (ct == 4) { R = G = B = r[x * 2]; A = r[x * 2 + 1]; }
                    else {                /* grey or palette, 1-8 bits per pixel */
                        int bit = x * depth, v = (r[bit >> 3] >> (8 - depth - (bit & 7))) & ((1 << depth) - 1);
                        if (ct == 3) { R = pal[v * 3]; G = pal[v * 3 + 1]; B = pal[v * 3 + 2]; }
                        else R = G = B = v * 255 / ((1 << depth) - 1);
                    }
                    o[ox] = rgb565s(R * A / 255, G * A / 255, B * A / 255);   /* over black */
                }
                for (int oy = y0 + 1; o && oy < y1; oy++) memcpy(d->px + oy * d->ow, o, d->ow * 2);
                uint8_t *t = prev; prev = cur; cur = t;
                fill = 0;
                y++;
            }
            win_pos = (win_pos + n_out) & (TINFL_LZ_DICT_SIZE - 1);
            if (st == TINFL_STATUS_DONE && y < H) err = "PNG data ended early";
        }
    }
    free(inb); free(cur); free(prev);
    return err;
}

/* GET url and decode the picture fitted into bw x bh; the HA token is sent only to HA.
 * Uses its own connection (streamed reads don't mix with the shared keep-alive one).
 * Returns the pixels (caller frees) or NULL. */
static uint16_t *fetch_picture(const char *url, bool auth, uint8_t fit, int bw, int bh, int *ow, int *oh, int *zoom, int pg, int row)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
    };
    /* the 43 KB PNG decoder state lives only while a picture loads; held statically it
     * left too little RAM for mbedTLS's 16 KB receive buffer on other requests */
    pic_dec_t d = { .win = malloc(TINFL_LZ_DICT_SIZE), .inf = malloc(sizeof(tinfl_decompressor)), .fit = fit };
    if (!d.win || !d.inf) {
        ESP_LOGW(TAG, "picture: no memory for the decoder");
        free(d.win); free(d.inf);
        return NULL;
    }
    d.http = esp_http_client_init(&cfg);
    if (!d.http) { free(d.win); free(d.inf); return NULL; }
    if (auth) esp_http_client_set_header(d.http, "Authorization", prov_bearer());
    const char *err = NULL, *path = auth ? url + strlen(prov_url()) : url;
    int st = -1;
    if (esp_http_client_open(d.http, 0) != ESP_OK || esp_http_client_fetch_headers(d.http) < 0) err = "connection failed";
    else if ((st = esp_http_client_get_status_code(d.http)) != 200) err = "HTTP error";
    else if ((d.head_len = pic_read(&d, d.head, 8)) != 8) err = "empty response";
    else if (!memcmp(d.head, "\x89PNG", 4)) err = png_decode(&d, bw, bh, pg, row);
    else if (d.head[0] == 0xFF && d.head[1] == 0xD8) err = jpg_decode(&d, bw, bh, pg, row);
    else err = "not a JPEG or PNG";
    esp_http_client_close(d.http);
    esp_http_client_cleanup(d.http);
    free(d.win); free(d.inf);
    if (err) {
        if (st > 0 && st != 200) { ESP_LOGW(TAG, "%s: HTTP %d", path, st); add_error("%.60s: HTTP %d", path, st); }
        else { ESP_LOGW(TAG, "%s: %s", path, err); add_error("%.60s: %s", path, err); }
        free(d.px);
        return NULL;
    }
    *ow = d.ow; *oh = d.oh; *zoom = d.zoom ? d.zoom : 1;
    ESP_LOGI(TAG, "picture %dx%d%s from %.60s", d.ow, d.oh, d.zoom ? " (half size, zoomed)" : "", path);
    return d.px;
}

static char s_pic_state[LAYOUT_MAX_ROWS][LAYOUT_IMAGE_LEN];   /* what was shown last */

/* Pictures of the page on screen. Cameras are re-read every time; image entities when their
 * state (the image's timestamp) changes; image URLs once per page visit. */
static void fetch_pictures(bool force)
{
    int pg;
    uint16_t w[LAYOUT_MAX_ROWS], h[LAYOUT_MAX_ROWS];
    taskENTER_CRITICAL(&s_pic_lock);
    pg = s_pic_page;
    memcpy(w, s_pic_w, sizeof(w));
    memcpy(h, s_pic_h, sizeof(h));
    taskEXIT_CRITICAL(&s_pic_lock);
    if (force) memset(s_pic_state, 0, sizeof(s_pic_state));
    if (pg < 0 || pg >= s_layout.count || !s_pic_cb) return;
    for (int i = 0; i < s_layout.pages[pg].count; i++) {
        const layout_item_t *it = &s_layout.pages[pg].items[i];
        if (it->kind != LAYOUT_PICTURE || !w[i] || !h[i]) continue;
        resp_t *r = &s_rbuf;   /* shared: all callers run on the HA task, one at a time */
        static char url[LAYOUT_IMAGE_LEN + 128];
        char path[128], name[32], key[LAYOUT_IMAGE_LEN] = "";
        snprintf(name, sizeof(name), "%.31s", it->name[0] ? it->name : it->entity);
        const char *src = it->image ? s_layout.images[it->image - 1] : it->entity;
        bool camera = is_domain(src, "camera"), entity = camera || is_domain(src, "image");
        snprintf(path, sizeof(path), "/api/states/%s", it->entity);
        if (it->entity[0] && ha_request(HTTP_METHOD_GET, path, NULL, r) == 200) {   /* picture-glance: no entity */
            if (!it->name[0] && json_str(r->buf, "\"friendly_name\":\"", name, sizeof(name))) utf8_trim(name);
            if (!it->image) json_str(r->buf, "\"state\":\"", key, sizeof(key));
        }
        if (it->image && !entity) snprintf(key, sizeof(key), "%s", src);
        if (!camera && key[0] && !strcmp(key, s_pic_state[i])) continue;   /* same image */
        bool auth = true;
        if (camera) snprintf(url, sizeof(url), "%s/api/camera_proxy/%s?width=%d&height=%d", prov_url(), src, w[i], h[i]);
        else if (entity) snprintf(url, sizeof(url), "%s/api/image_proxy/%s", prov_url(), src);
        else if (strstr(src, "://")) { snprintf(url, sizeof(url), "%s", src); size_t ul = strlen(prov_url()); auth = !strncmp(src, prov_url(), ul) && src[ul] == '/'; }
        else snprintf(url, sizeof(url), "%s%s%s", prov_url(), src[0] == '/' ? "" : "/", src);
        int ow, oh, zoom;
        if (s_http) {                     /* two TLS sessions at once leave too little RAM to decode */
            esp_http_client_cleanup(s_http);
            s_http = NULL;
        }
        uint16_t *px = fetch_picture(url, auth, it->flags, w[i], h[i], &ow, &oh, &zoom, pg, i);
        if (!px) continue;
        snprintf(s_pic_state[i], sizeof(s_pic_state[i]), "%s", key);
        s_pic_cb(pg, i, px, ow, oh, zoom, name);
    }
}

void ha_client_show_page(int page, const uint16_t w[LAYOUT_MAX_ROWS], const uint16_t h[LAYOUT_MAX_ROWS])
{
    taskENTER_CRITICAL(&s_pic_lock);
    s_pic_page = page;
    memcpy(s_pic_w, w, sizeof(s_pic_w));
    memcpy(s_pic_h, h, sizeof(s_pic_h));
    taskEXIT_CRITICAL(&s_pic_lock);
    bool pics = false;                    /* no pictures here: nothing to fetch (and the */
    for (int i = 0; i < LAYOUT_MAX_ROWS; i++) pics |= w[i] && h[i];   /* live websocket stays) */
    if (pics && s_events) xEventGroupSetBits(s_events, PIC_BIT);
}


/* ---- Live updates ------------------------------------------------------------------
 * Between the HTTP rounds (graph history, pictures, layout; every HA_LIVE_RESYNC_MS) one
 * websocket stays open with subscribe_entities for the dashboard and settings entities, so
 * changes show at once. Only one TLS session fits in RAM: the websocket is closed for the
 * HTTP rounds and opened again after them (the new subscription starts with a full
 * snapshot, so nothing is missed). Service calls go over the websocket while it's open. */
static live_t *s_live;
static QueueHandle_t s_live_q;
static volatile bool s_live_lost;      /* an event was dropped: resync over HTTP */
static int s_lp_on = -1, s_lp_secs;    /* presentation entities, as last received */

/* websocket task: queue the message for the HA task */
static void live_event(char *msg, void *arg)
{
    (void)arg;
    if (!s_live_q || xQueueSend(s_live_q, &msg, 0) != pdTRUE) { free(msg); s_live_lost = true; }
    xEventGroupSetBits(s_events, LIVE_BIT);
}

static void live_stop(void)
{
    if (s_lws) {
        dash_ws_close(s_lws);             /* no more events after this */
        s_lws = NULL;
        ESP_LOGI(TAG, "live: closed");
    }
    char *m;
    while (s_live_q && xQueueReceive(s_live_q, &m, 0) == pdTRUE) free(m);
    live_free(s_live);
    s_live = NULL;
    xEventGroupClearBits(s_events, LIVE_BIT);
}

static bool live_kind(layout_kind_t k)
{
    return k == LAYOUT_VALUES || k == LAYOUT_GAUGE || k == LAYOUT_GLANCE || k == LAYOUT_ENTITIES || k == LAYOUT_GRAPH ||
           k == LAYOUT_BUTTONS || k == LAYOUT_WEATHER || k == LAYOUT_THERMOSTAT || k == LAYOUT_MEDIA || k == LAYOUT_LIGHT || k == LAYOUT_TODO;
}

static bool live_start(void)
{
    if (s_lws) return true;
    if (!s_live_q && !(s_live_q = xQueueCreate(16, sizeof(char *)))) return false;
    static const char *ids[LAYOUT_MAX_PAGES * LAYOUT_MAX_ROWS + 1 + H_COUNT + LAYOUT_MAX_CONDS];   /* + name, board helpers, conditions */
    int n = 0;
    ids[n++] = s_name_ent;
    if (s_board[0]) {
        ids[n++] = s_ent.theme;
        ids[n++] = s_ent.land;
        ids[n++] = s_ent.bri;
        ids[n++] = s_ent.pres;
        ids[n++] = s_ent.pres_int;
        ids[n++] = s_ent.dash;
        ids[n++] = s_ent.view;
        ids[n++] = s_ent.page;
        ids[n++] = s_ent.night;
        ids[n++] = s_ent.night_bri;
        ids[n++] = s_ent.scr_to;
        ids[n++] = s_ent.notify;
        ids[n++] = s_ent.autorot;
        ids[n++] = s_ent.motion;
        ids[n++] = s_ent.saver;
    }
    for (int pg = 0; pg < s_layout.count; pg++)
        for (int i = 0; i < s_layout.pages[pg].count; i++) {
            const layout_item_t *it = &s_layout.pages[pg].items[i];
            if (!live_kind(it->kind) || !it->entity[0]) continue;
            bool dup = false;
            for (int k = 0; k < n && !dup; k++) dup = !strcmp(ids[k], it->entity);
            if (!dup) ids[n++] = it->entity;
        }
    for (int k = 0; k < LAYOUT_MAX_CONDS; k++) {   /* hidden cards' conditions too */
        const char *e = s_layout.conds[k].entity;
        bool dup = !e[0];
        for (int j = 0; j < n && !dup; j++) dup = !strcmp(ids[j], e);
        if (!dup) ids[n++] = e;
    }
    s_live = live_new(ids, n);
    char *req = malloc(64 + n * 84);
    if (!s_live || !req) { free(req); live_stop(); return false; }
    int w = sprintf(req, "\"type\":\"subscribe_entities\",\"entity_ids\":[");
    for (int k = 0; k < n; k++) w += sprintf(req + w, "%s\"%s\"", k ? "," : "", ids[k]);
    strcpy(req + w, "]");
    if (s_http) {                         /* two TLS sessions at once don't fit in RAM */
        esp_http_client_cleanup(s_http);
        s_http = NULL;
    }
    s_lws = dash_ws_open();
    char *res = NULL;
    if (s_lws) {
        dash_ws_set_event_cb(s_lws, live_event, NULL);
        res = dash_ws_call(s_lws, req);
    }
    bool ok = res && strstr(res, "\"success\":true");
    free(res);
    free(req);
    if (!ok) { ESP_LOGW(TAG, "live: subscribe failed"); live_stop(); return false; }
    ESP_LOGI(TAG, "live: %d entities (free %u, largest %u)", n, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return true;
}

static void live_dispatch(const char *id, const char *json)
{
    char st[16] = "";
    json_str(json, "\"state\":\"", st, sizeof(st));
    {   /* a condition's entity (may also be a card's: go on) */
        char cs[LAYOUT_COND_LEN];
        bool known = json_str(json, "\"state\":\"", cs, sizeof(cs));
        layout_mask_t m = cond_bit(&s_layout, id, known ? cs : NULL);
        if (m != s_cond_true) { s_cond_true = m; s_cond_dirty = true; }
    }
    if (!strcmp(id, s_name_ent)) {
        char name[32];
        board_parse(json, name, sizeof(name));
        if (strcmp(name, s_board)) {      /* new prefix: an HTTP round resubscribes */
            s_name_changed = true;
            xEventGroupSetBits(s_events, REFRESH_BIT);
        }
        return;
    }
    if (!s_board[0]) {
        /* no helpers subscribed */
    } else if (!strcmp(id, s_ent.theme)) {
        ha_state_t s = !strcmp(st, "on") ? HA_STATE_ON : !strcmp(st, "off") ? HA_STATE_OFF : HA_STATE_UNKNOWN;
        if ((s == HA_STATE_ON || s == HA_STATE_OFF) && s != s_rec_theme) { s_rec_theme = s; s_rec_dirty = true; }
        if (s_state_cb) s_state_cb(s);
    } else if (!strcmp(id, s_ent.land)) {
        if ((!strcmp(st, "on") || !strcmp(st, "off")) && s_land_cb) s_land_cb(!strcmp(st, "on"));
    } else if (!strcmp(id, s_ent.bri)) {
        parse_brightness(json);
    } else if (!strcmp(id, s_ent.pres) || !strcmp(id, s_ent.pres_int)) {
        float secs;
        if (!strcmp(id, s_ent.pres)) s_lp_on = !strcmp(st, "on") ? 1 : !strcmp(st, "off") ? 0 : s_lp_on;
        else if (json_num(json, "\"state\":\"", &secs)) s_lp_secs = (int)secs;
        if (s_lp_on >= 0 && s_pres_cb) s_pres_cb(s_lp_on, s_lp_secs);
    } else if (!strcmp(id, s_ent.night) || !strcmp(id, s_ent.night_bri) || !strcmp(id, s_ent.scr_to)) {
        float v;
        if (!strcmp(id, s_ent.night)) s_ln_night = !strcmp(st, "on") ? 1 : !strcmp(st, "off") ? 0 : s_ln_night;
        else if (json_num(json, "\"state\":\"", &v) && v >= 0) *(!strcmp(id, s_ent.night_bri) ? &s_ln_pct : &s_ln_to) = (int)(v + 0.5f);
        if (s_night_cb) s_night_cb(s_ln_night, s_ln_pct, s_ln_to);
    } else if (!strcmp(id, s_ent.autorot)) {
        if (s_autorot_cb && (!strcmp(st, "on") || !strcmp(st, "off"))) s_autorot_cb(!strcmp(st, "on"));
    } else if (!strcmp(id, s_ent.motion)) {
        if (s_motion_cb && (!strcmp(st, "on") || !strcmp(st, "off"))) s_motion_cb(!strcmp(st, "on"));
    } else if (!strcmp(id, s_ent.saver)) {
        if (s_saver_cb && (!strcmp(st, "on") || !strcmp(st, "off"))) s_saver_cb(!strcmp(st, "on"));
    } else if (!strcmp(id, s_ent.dash)) {
        s_nav_dash_ok = nav_text(json, s_nav_dash, sizeof(s_nav_dash));
        nav_report();
    } else if (!strcmp(id, s_ent.view)) {
        s_nav_view_ok = nav_text(json, s_nav_view, sizeof(s_nav_view));
        nav_report();
    } else if (!strcmp(id, s_ent.page)) {
        s_nav_page = nav_page(json);
        nav_report();
    }
    for (int pg = 0; pg < s_layout.count; pg++)
        for (int i = 0; i < s_layout.pages[pg].count; i++) {
            const layout_item_t *it = &s_layout.pages[pg].items[i];
            if (strcmp(it->entity, id) || !live_kind(it->kind)) continue;
            char text[24], unit[8], name[32];
            switch (it->kind) {
            case LAYOUT_BUTTONS: parse_button(pg, i, json); break;
            case LAYOUT_WEATHER: parse_weather(pg, i, json); break;
            case LAYOUT_THERMOSTAT: parse_climate(pg, i, json); break;
            case LAYOUT_MEDIA: parse_media(pg, i, json); break;
            case LAYOUT_LIGHT: parse_light(pg, i, json); break;
            case LAYOUT_TODO: fetch_todo(pg, i); break;   /* state = open items: read the list */
            case LAYOUT_GRAPH:
                parse_entity(pg, i, json, text, unit, name);
                emit_graph(pg, i, text, unit, name, NULL, 0, 0, it->hours, !strcmp(unit, "%"));
                break;
            default:
                parse_entity(pg, i, json, text, unit, name);
                emit_sensor(pg, i, text, unit, name);
            }
        }
}

/* HA task: apply the queued events */
static void live_drain(void)
{
    static char json[512];
    int ch[LAYOUT_MAX_PAGES * LAYOUT_MAX_ROWS + 1 + H_COUNT + LAYOUT_MAX_CONDS];   /* as many as live_start subscribes */
    char *m;
    while (s_live_q && xQueueReceive(s_live_q, &m, 0) == pdTRUE) {
        int n = s_live ? live_apply(s_live, m, ch, sizeof(ch) / sizeof(ch[0])) : 0;
        free(m);
        for (int k = 0; k < n; k++) {
            if (s_board[0] && !strcmp(live_id(s_live, ch[k]), s_ent.notify)) {
                static notify_seen_t seen;   /* kept across resyncs */
                char text[NOTIFY_MAX];
                if (notify_new(&seen, live_text(s_live, ch[k]), text, sizeof(text)) && s_notify_cb) s_notify_cb(text);
                continue;
            }
            if (live_json(s_live, ch[k], json, sizeof(json))) live_dispatch(live_id(s_live, ch[k]), json);
        }
    }
}

/* POST /api/services/<domain>/<service> with body: over the live websocket when it's open */
static int ha_service(const char *path, const char *body)
{
    char dom[32], svc[32];
    if (dash_ws_alive(s_lws) && sscanf(path, "/api/services/%31[^/]/%31[^?/]", dom, svc) == 2) {
        char *req = malloc(strlen(body) + 128);
        if (req) {
            sprintf(req, "\"type\":\"call_service\",\"domain\":\"%s\",\"service\":\"%s\",\"service_data\":%s", dom, svc, body);
            char *res = dash_ws_call(s_lws, req);
            free(req);
            int st = res && strstr(res, "\"success\":true") ? 200 : res ? 400 : -1;
            free(res);
            if (st > 0) return st;
            ESP_LOGW(TAG, "live: %s failed, using HTTP", path);
        }
        live_stop();
    }
    return ha_request(HTTP_METHOD_POST, path, body, NULL);
}

/* Settings and button taps: service calls only. True when all succeeded. */
static bool do_user(EventBits_t b)
{
    char path[96];
    char body[128];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", s_ent.theme);
    bool ok = true;
    if (!s_board[0]) {                    /* no name: no helpers to write */
        b &= ~(SETBRI_BIT | PRES_BIT | NIGHT_BIT | NAV_BIT | ORIENT_BIT | TOGGLE_BIT | AUTOROT_BIT | MOTION_BIT | SAVER_BIT);
        s_nav_writing = false;
    }
    if (!has(H_BRI)) b &= ~SETBRI_BIT;    /* missing helpers: the board keeps the setting */
    if (!has(H_PRES) && !has(H_PRES_INT)) b &= ~PRES_BIT;
    if (!has(H_NIGHT) && !has(H_NIGHT_BRI) && !has(H_SCR_TO)) b &= ~NIGHT_BIT;
    if (!has(H_DASH) && !has(H_VIEW) && !has(H_PAGE)) { b &= ~NAV_BIT; s_nav_writing = false; }
    if (!has(H_LAND)) b &= ~ORIENT_BIT;
    if (!has(H_AUTOROT)) b &= ~AUTOROT_BIT;
    if (!has(H_MOTION)) b &= ~MOTION_BIT;
    if (!has(H_SAVER)) b &= ~SAVER_BIT;
    if (!has(H_THEME)) b &= ~TOGGLE_BIT;
    if (!s_board[0] || !has(H_NOTIFY)) b &= ~NOTIFY_BIT;
    if (b & SETBRI_BIT) {
        char sb[160];
        snprintf(sb, sizeof(sb), "{\"entity_id\":\"%s\",\"value\":%.1f}", s_ent.bri, (double)s_set_value);
        ESP_LOGI(TAG, "set brightness %.1f", (double)s_set_value);
        int st = ha_service("/api/services/input_number/set_value", sb);
        ok &= st >= 200 && st < 300;
    }
    if (b & PRES_BIT) {
        bool on = s_set_pres_on;
        char sb[160], pb[128];
        snprintf(pb, sizeof(pb), "{\"entity_id\":\"%s\"}", s_ent.pres);
        snprintf(sb, sizeof(sb), "{\"entity_id\":\"%s\",\"value\":%d}", s_ent.pres_int, s_set_pres_secs);
        int st = has(H_PRES_INT) ? ha_service("/api/services/input_number/set_value", sb) : 200;
        ok &= st >= 200 && st < 300;
        st = has(H_PRES) ? ha_service(on ? "/api/services/input_boolean/turn_on" : "/api/services/input_boolean/turn_off",
                                      pb) : 200;
        ok &= st >= 200 && st < 300;
    }
    if (b & AUTOROT_BIT) {
        char sb[128];
        snprintf(sb, sizeof(sb), "{\"entity_id\":\"%s\"}", s_ent.autorot);
        int st = ha_service(s_set_autorot ? "/api/services/input_boolean/turn_on" : "/api/services/input_boolean/turn_off", sb);
        ok &= st >= 200 && st < 300;
    }
    if (b & MOTION_BIT) {
        char sb[128];
        snprintf(sb, sizeof(sb), "{\"entity_id\":\"%s\"}", s_ent.motion);
        int st = ha_service(s_set_motion ? "/api/services/input_boolean/turn_on" : "/api/services/input_boolean/turn_off", sb);
        ok &= st >= 200 && st < 300;
    }
    if (b & SAVER_BIT) {
        char sb[128];
        snprintf(sb, sizeof(sb), "{\"entity_id\":\"%s\"}", s_ent.saver);
        int st = ha_service(s_set_saver ? "/api/services/input_boolean/turn_on" : "/api/services/input_boolean/turn_off", sb);
        ok &= st >= 200 && st < 300;
    }
    if (b & NIGHT_BIT) {
        char sb[160];
        int st;
        snprintf(sb, sizeof(sb), "{\"entity_id\":\"%s\"}", s_ent.night);
        st = has(H_NIGHT) ? ha_service(s_set_night ? "/api/services/input_boolean/turn_on" : "/api/services/input_boolean/turn_off", sb) : 200;
        ok &= st >= 200 && st < 300;
        snprintf(sb, sizeof(sb), "{\"entity_id\":\"%s\",\"value\":%d}", s_ent.night_bri, s_set_night_pct);
        st = has(H_NIGHT_BRI) ? ha_service("/api/services/input_number/set_value", sb) : 200;
        ok &= st >= 200 && st < 300;
        snprintf(sb, sizeof(sb), "{\"entity_id\":\"%s\",\"value\":%d}", s_ent.scr_to, s_set_scr_to);
        st = has(H_SCR_TO) ? ha_service("/api/services/input_number/set_value", sb) : 200;
        ok &= st >= 200 && st < 300;
    }
    if (b & NOTIFY_BIT) {                 /* empty again: the same text later shows again */
        char sb[128];
        snprintf(sb, sizeof(sb), "{\"entity_id\":\"%s\",\"value\":\"\"}", s_ent.notify);
        int st = ha_service("/api/services/input_text/set_value", sb);
        ok &= st >= 200 && st < 300;
    }
    if (b & NAV_BIT) {
        char d[32], v[32], sb[160];
        taskENTER_CRITICAL(&s_view_lock);
        memcpy(d, s_wnav_dash, sizeof(d));
        memcpy(v, s_wnav_view, sizeof(v));
        int pg = s_wnav_page;
        taskEXIT_CRITICAL(&s_view_lock);
        ESP_LOGI(TAG, "nav: dashboard '%s', view '%s', page %d", d, v, pg + 1);
        snprintf(sb, sizeof(sb), "{\"entity_id\":\"%s\",\"value\":\"%s\"}", s_ent.dash, d);
        int st = ha_service("/api/services/input_text/set_value", sb);
        if (st >= 200 && st < 300) { snprintf(s_nav_dash, sizeof(s_nav_dash), "%s", d); s_nav_dash_ok = true; }
        snprintf(sb, sizeof(sb), "{\"entity_id\":\"%s\",\"value\":\"%s\"}", s_ent.view, v);
        st = ha_service("/api/services/input_text/set_value", sb);
        if (st >= 200 && st < 300) { snprintf(s_nav_view, sizeof(s_nav_view), "%s", v); s_nav_view_ok = true; }
        if (pg >= 0) {
            snprintf(sb, sizeof(sb), "{\"entity_id\":\"%s\",\"value\":%d}", s_ent.page, pg + 1);
            st = ha_service("/api/services/input_number/set_value", sb);
            if (st >= 200 && st < 300) s_nav_page = pg;
        }
        /* missing helpers (optional) give errors: not a failure of the user's action */
        s_nav_wrote = xTaskGetTickCount();
        if (!(xEventGroupGetBits(s_events) & NAV_BIT)) s_nav_writing = false;
    }
    if (b & ORIENT_BIT) {
        bool on = s_set_landscape;
        char lb[128];
        snprintf(lb, sizeof(lb), "{\"entity_id\":\"%s\"}", s_ent.land);
        ESP_LOGI(TAG, "%s %s", s_ent.land, on ? "on" : "off");
        int st = ha_service(on ? "/api/services/input_boolean/turn_on" : "/api/services/input_boolean/turn_off", lb);
        ok &= st >= 200 && st < 300;
    }
    if (b & TOGGLE_BIT) {
        bool on = s_set_dark;
        const char *dot = strchr(s_ent.theme, '.');
        snprintf(path, sizeof(path), "/api/services/%.*s/turn_%s", (int)(dot - s_ent.theme), s_ent.theme, on ? "on" : "off");
        ESP_LOGI(TAG, "%s %s", s_ent.theme, on ? "on" : "off");
        int st = ha_service(path, body);
        ok &= st >= 200 && st < 300;
    }
    if (b & PRESS_BIT) {
        int pg = s_press_page, row = s_press_row;
        bool pok = press_button(pg, row);
        if (s_press_cb) s_press_cb(pg, row, pok);
        vTaskDelay(pdMS_TO_TICKS(300)); /* let HA switch before the state is re-read */
    }
    if (b & LIGHT_BIT) {
        int pg = s_lt_page, row = s_lt_row, pct = s_lt_pct;
        if (pg >= 0 && pg < s_layout.count && row >= 0 && row < s_layout.pages[pg].count &&
            s_layout.pages[pg].items[row].kind == LAYOUT_LIGHT) {
            const char *e = s_layout.pages[pg].items[row].entity;
            char lb[112];
            if (pct > 0) snprintf(lb, sizeof(lb), "{\"entity_id\":\"%s\",\"brightness_pct\":%d}", e, pct);
            else snprintf(lb, sizeof(lb), "{\"entity_id\":\"%s\"}", e);
            const char *svc = pct > 0 ? "/api/services/light/turn_on" : pct == 0 ? "/api/services/light/turn_off" : "/api/services/light/toggle";
            ESP_LOGI(TAG, "%s -> %s (%d)", e, svc, pct);
            int st = ha_service(svc, lb);
            ok &= st >= 200 && st < 300;
        }
    }
    if (b & TODO_BIT) {
        static todo_req_t q[8];
        taskENTER_CRITICAL(&s_todo_lock);
        int n = s_todo_n;
        memcpy(q, s_todo_q, n * sizeof(q[0]));
        s_todo_n = 0;
        taskEXIT_CRITICAL(&s_todo_lock);
        uint64_t reread = 0;              /* bit pg * LAYOUT_MAX_ROWS + row: lists to read back */
        for (int k = 0; k < n; k++) {
            int pg = q[k].page, row = q[k].row;
            if (pg < 0 || pg >= s_layout.count || row < 0 || row >= s_layout.pages[pg].count ||
                s_layout.pages[pg].items[row].kind != LAYOUT_TODO) continue;
            char tb[192];
            snprintf(tb, sizeof(tb), "{\"entity_id\":\"%s\",\"item\":\"%s\",\"status\":\"%s\"}",
                     s_layout.pages[pg].items[row].entity, q[k].uid, q[k].done ? "completed" : "needs_action");
            int st = ha_service("/api/services/todo/update_item", tb);
            ok &= st >= 200 && st < 300;
            reread |= 1ull << (pg * LAYOUT_MAX_ROWS + row);
        }
        for (int g = 0; g < LAYOUT_MAX_PAGES * LAYOUT_MAX_ROWS; g++)   /* as HA has them now (also undoes a failed tap) */
            if (reread >> g & 1) fetch_todo(g / LAYOUT_MAX_ROWS, g % LAYOUT_MAX_ROWS);
    }
    if (b & MEDIA_BIT) {
        int pg = s_med_page, row = s_med_row;
        if (pg >= 0 && pg < s_layout.count && row >= 0 && row < s_layout.pages[pg].count &&
            s_layout.pages[pg].items[row].kind == LAYOUT_MEDIA) {
            char mb[96];
            const char *svc = s_med_cmd == HA_MEDIA_NEXT ? "/api/services/media_player/media_next_track"
                                                         : "/api/services/media_player/media_play_pause";
            snprintf(mb, sizeof(mb), "{\"entity_id\":\"%s\"}", s_layout.pages[pg].items[row].entity);
            ESP_LOGI(TAG, "%s -> %s", s_layout.pages[pg].items[row].entity, svc);
            int st = ha_service(svc, mb);
            ok &= st >= 200 && st < 300;
        }
    }
    if (b & TARGET_BIT) {
        int pg = s_tgt_page, row = s_tgt_row;
        if (pg >= 0 && pg < s_layout.count && row >= 0 && row < s_layout.pages[pg].count &&
            s_layout.pages[pg].items[row].kind == LAYOUT_THERMOSTAT) {
            char tb[112];
            snprintf(tb, sizeof(tb), "{\"entity_id\":\"%s\",\"temperature\":%.1f}", s_layout.pages[pg].items[row].entity, s_tgt_value);
            ESP_LOGI(TAG, "%s target %.1f", s_layout.pages[pg].items[row].entity, s_tgt_value);
            int st = ha_service("/api/services/climate/set_temperature", tb);
            ok &= st >= 200 && st < 300;
        }
    }
    return ok;
}

static bool has_pictures(void)
{
    for (int pg = 0; pg < s_layout.count; pg++)
        for (int i = 0; i < s_layout.pages[pg].count; i++)
            if (s_layout.pages[pg].items[i].kind == LAYOUT_PICTURE) return true;
    return false;
}

static void ha_task(void *arg)
{
    board_report();                       /* the cached name, or the setup screen */

    {   /* start with the cached layout, if any */
        if (layout_store_load(&s_scratch)) {
            s_full_hash = layout_hash(&s_scratch);
            s_cond_true = cond_load();       /* as last seen, until HA answers */
            layout_visible(&s_scratch, s_cond_true);
            use_layout(&s_scratch);
            cache_replay();               /* the last data, until HA answers */
        }
    }
    TickType_t layout_at = 0, pic_at = 0, round_at = 0;
    bool layout_read = false;

    for (;;) {
        xEventGroupWaitBits(s_events, WIFI_UP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        /* wait for a tap or a live event; poll every HA_POLL_INTERVAL_MS without the live
         * websocket, else an HTTP round every HA_LIVE_RESYNC_MS (pictures: their interval) */
        TickType_t wait = pdMS_TO_TICKS(HA_POLL_INTERVAL_MS), now = xTaskGetTickCount();
        if (s_lws) {                      /* deadlines: live events must not push them back */
            TickType_t due = round_at + pdMS_TO_TICKS(HA_LIVE_RESYNC_MS);
            if (has_pictures() && pic_at + pdMS_TO_TICKS(HA_PICTURE_INTERVAL_MS) < due) due = pic_at + pdMS_TO_TICKS(HA_PICTURE_INTERVAL_MS);
            wait = (int32_t)(due - now) > 0 ? due - now : 0;
        }
        const EventBits_t waited = USER_BITS | WAKE_BIT | REFRESH_BIT | PIC_BIT | LIVE_BIT | PAUSE_BIT;
        EventBits_t b = xEventGroupWaitBits(s_events, waited, pdTRUE, pdFALSE, wait) & waited;   /* not WIFI_UP_BIT */
        if (b & PAUSE_BIT) {              /* an OTA download needs the RAM of our TLS session */
            live_stop();
            if (s_http) { esp_http_client_cleanup(s_http); s_http = NULL; }
            ESP_LOGI(TAG, "paused");
            xEventGroupSetBits(s_events, PAUSED_BIT);
            xEventGroupWaitBits(s_events, RESUME_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
            xEventGroupClearBits(s_events, PAUSED_BIT);
            ESP_LOGI(TAG, "resumed");
            xEventGroupSetBits(s_events, b & ~PAUSE_BIT ? b & ~PAUSE_BIT : WAKE_BIT);   /* redo now */
            continue;
        }
        bool timeout = !b;
        if (s_lws && !timeout) {          /* events came in, but a round may be due anyway */
            TickType_t t = xTaskGetTickCount();
            timeout = t - round_at >= pdMS_TO_TICKS(HA_LIVE_RESYNC_MS) ||
                      (has_pictures() && t - pic_at >= pdMS_TO_TICKS(HA_PICTURE_INTERVAL_MS));
        }
        s_err[0] = 0;
        if (s_lws && (!dash_ws_alive(s_lws) || s_live_lost)) {
            ESP_LOGW(TAG, "live: %s, resync over HTTP", s_live_lost ? "events dropped" : "connection lost");
            s_live_lost = false;
            live_stop();
            timeout = true;               /* an HTTP round now */
        }
        if (s_lws) {
            if (b & LIVE_BIT) live_drain();
            b &= ~LIVE_BIT;
            if (b & USER_BITS && !(b & ~USER_BITS)) {   /* taps only: over the websocket */
                /* no busy indicator: the HA dot stays steady green while live */
                bool ok = do_user(b);
                bool updated = b & (TOGGLE_BIT | ORIENT_BIT | SETBRI_BIT | PRES_BIT | NIGHT_BIT | AUTOROT_BIT | MOTION_BIT | SAVER_BIT);
                if (updated && s_done_cb) s_done_cb(ok, false);
                if (s_err_cb) s_err_cb(s_err);
                continue;
            }
            if (s_cond_dirty) {           /* other cards shown: an HTTP round resubscribes */
                b |= WAKE_BIT;
                timeout = true;
            }
            if (!b && !timeout) continue;                /* only events */
            /* due: just the pictures, unless a full round is due */
            if (!b && xTaskGetTickCount() - round_at < pdMS_TO_TICKS(HA_LIVE_RESYNC_MS)) b = PIC_BIT;
        }
        if (s_lws) ESP_LOGI(TAG, "live: HTTP round (bits 0x%x%s)", (unsigned)b, timeout ? ", timeout" : "");
        /* a background round between live periods doesn't pulse the HA dot (the data is
         * current); polling without the websocket, or the refresh button, does */
        bool quiet = s_lws && !(b & REFRESH_BIT);
        bool was_live = s_lws != NULL;
        live_stop();                      /* HTTP from here: one TLS session at a time */
        if (b == PIC_BIT) {               /* only a page switch: just its pictures */
            fetch_pictures(true);
            pic_at = xTaskGetTickCount();
            live_start();
            continue;
        }
        round_at = xTaskGetTickCount();
        if (s_busy_cb && !quiet) s_busy_cb(true);
        bool user = b & (TOGGLE_BIT | ORIENT_BIT | SETBRI_BIT | PRESS_BIT | TARGET_BIT | MEDIA_BIT | LIGHT_BIT | TODO_BIT | PRES_BIT | NIGHT_BIT | AUTOROT_BIT | MOTION_BIT | SAVER_BIT);
        bool refresh = b & REFRESH_BIT;   /* the refresh button also re-reads the dashboard */
        /* not while a tap waits: the dashboard check can take seconds */
        if (refresh && s_hold_next) { s_hold_next = false; s_hold = true; }
        if (refresh || (!user && (!layout_read || xTaskGetTickCount() - layout_at >= pdMS_TO_TICKS(HA_LAYOUT_INTERVAL_MS)))) {
            refresh_layout(refresh);
            layout_at = xTaskGetTickCount();
            layout_read = true;
        }
        if (!was_live && s_layout.conds[0].entity[0]) {   /* polling: no live events, read the conditions */
            layout_mask_t before = s_cond_true;
            cond_seed(&s_layout);
            if (s_cond_true != before) s_cond_dirty = true;
        }
        if (s_cond_dirty) cond_apply();
        bool updated = b & (TOGGLE_BIT | ORIENT_BIT | SETBRI_BIT | REFRESH_BIT | PRES_BIT | NIGHT_BIT | AUTOROT_BIT | MOTION_BIT | SAVER_BIT);
        bool ok = true;
        ok &= do_user(b);
        if (updated) {
            vTaskDelay(pdMS_TO_TICKS(300)); /* give HA a moment to update the state */
            s_force_report = true;         /* resync the UI even if nothing changed */
        }
        if (s_icon_rules_pending) load_icon_rules();
        s_name_changed = false;
        if (fetch_name()) s_force_report = true;   /* new helpers: show their values */
        bool fetched = fetch_state();
        if (s_mkhelp && fetched) create_helpers();
        {   /* card data to flash: a minute after start, then at most every HA_CACHE_INTERVAL_MS */
            static TickType_t saved_at;
            static bool saved;
            TickType_t now = xTaskGetTickCount();
            if (s_rec_dirty && fetched && (saved ? now - saved_at >= pdMS_TO_TICKS(HA_CACHE_INTERVAL_MS)
                                                 : now >= pdMS_TO_TICKS(60000))) {
                cache_save();
                saved = true;
                saved_at = now;
            }
        }
        {   /* device diagnostics: once Wi-Fi is up, then every HA_DIAG_INTERVAL_MS */
            static TickType_t diag_at;
            static bool diag_sent;
            TickType_t now = xTaskGetTickCount();
            if (fetched && !user_waiting() && (!diag_sent || now - diag_at >= pdMS_TO_TICKS(HA_DIAG_INTERVAL_MS))) {
                report_diag();
                diag_sent = true;
                diag_at = now;
            } else if (fetched && diag_sent && !user_waiting()) {
                report_orientation(false);   /* in between: only when it changed */
            }
        }
        hold_release();                   /* with the states: graphs and pictures follow on the new view */
        for (int pg = 0; pg < s_layout.count && !user_waiting(); pg++)
            for (int i = 0; i < s_layout.pages[pg].count && !user_waiting(); i++)
                if (s_layout.pages[pg].items[i].kind == LAYOUT_GRAPH) fetch_graph(pg, i);
        bool pic_force = b & (PIC_BIT | REFRESH_BIT);
        if (!user_waiting() && (pic_force || xTaskGetTickCount() - pic_at >= pdMS_TO_TICKS(HA_PICTURE_INTERVAL_MS))) {
            fetch_pictures(pic_force);
            pic_at = xTaskGetTickCount();
        }
        if (b & REFRESH_BIT) ok &= fetched;
        if (updated) {
            ESP_LOGI(TAG, "update %s", ok ? "succeeded" : "FAILED");
            if (s_done_cb) s_done_cb(ok, refresh);
        }
        if (fetched) report_errors();     /* HA reachable: log this round's errors there */
        if (s_err_cb) s_err_cb(s_err);
        live_start();                     /* back to live updates until the next round */
        if (s_busy_cb && !quiet) s_busy_cb(false);
    }
}

/* Re-send the last data of every card, the theme and the brightness through the callbacks */
void ha_client_replay(void)
{
    if (s_hold) return;                   /* the records are the held layout's: not the UI's */
    rec_lock();
    if (s_rec_bri_has && s_brightness_cb) s_brightness_cb(s_rec_bri[0], s_rec_bri[1], s_rec_bri[2]);
    for (int pg = 0; pg < s_layout.count; pg++)
        for (int row = 0; row < s_layout.pages[pg].count; row++)
            for (int t = 0; t < REC_TYPES; t++) {
                const uint8_t *d = s_rec[pg][row][t];
                if (d) rec_emit(t, pg, row, d, d + s_rec_len[pg][row][t], s_rec_len[pg][row][t]);
            }
    rec_unlock();
}

void ha_client_start(ha_state_cb_t cb, ha_brightness_cb_t bcb, ha_done_cb_t dcb, ha_sensor_cb_t scb, ha_graph_cb_t gcb)
{
    if (!s_rec_mx) s_rec_mx = xSemaphoreCreateRecursiveMutex();
    s_graph_cb = gcb;
    s_sensor_cb = scb;
    s_done_cb = dcb;
    s_state_cb = cb;
    s_brightness_cb = bcb;
    s_events = xEventGroupCreate();
    board_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, prov_ssid(), sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, prov_pass(), sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    const esp_timer_create_args_t ta = { .callback = nowifi_fired, .name = "nowifi" };
    if (esp_timer_create(&ta, &s_nowifi_timer) == ESP_OK)
        esp_timer_start_once(s_nowifi_timer, (uint64_t)HA_WIFI_PORTAL_S * 1000000);

    xTaskCreate(ha_task, "ha", 8192, NULL, 3, NULL);   /* also rebuilds the UI: 8 pages measured ~3.7 KB peak */
}

void ha_client_todo_set(int page, int row, const char *uid, bool done)
{
    taskENTER_CRITICAL(&s_todo_lock);
    int k = 0;                            /* the same item again: its latest state */
    while (k < s_todo_n && (s_todo_q[k].page != page || s_todo_q[k].row != row || strcmp(s_todo_q[k].uid, uid))) k++;
    if (k == s_todo_n && s_todo_n < (int)(sizeof(s_todo_q) / sizeof(s_todo_q[0]))) s_todo_n++;
    if (k < s_todo_n) {
        s_todo_q[k].page = (int8_t)page;
        s_todo_q[k].row = (int8_t)row;
        s_todo_q[k].done = done;
        snprintf(s_todo_q[k].uid, sizeof(s_todo_q[k].uid), "%s", uid);
    }
    taskEXIT_CRITICAL(&s_todo_lock);
    if (s_events) xEventGroupSetBits(s_events, TODO_BIT);
}

void ha_client_light(int page, int row, int pct)
{
    s_lt_page = page;
    s_lt_row = row;
    s_lt_pct = pct;
    if (s_events) xEventGroupSetBits(s_events, LIGHT_BIT);
}

void ha_client_media(int page, int row, ha_media_cmd_t cmd)
{
    s_med_page = page;
    s_med_row = row;
    s_med_cmd = cmd;
    if (s_events) xEventGroupSetBits(s_events, MEDIA_BIT);
}

void ha_client_set_target(int page, int row, float target)
{
    s_tgt_page = page;
    s_tgt_row = row;
    s_tgt_value = target;
    if (s_events) xEventGroupSetBits(s_events, TARGET_BIT);
}

void ha_client_press(int page, int row)
{
    s_press_page = page;
    s_press_row = row;
    if (s_events) xEventGroupSetBits(s_events, PRESS_BIT);
}

void ha_client_set_theme(bool dark)
{
    s_set_dark = dark;
    if (s_events) xEventGroupSetBits(s_events, TOGGLE_BIT);
}

void ha_client_set_presentation(bool on, int secs)
{
    s_set_pres_on = on;
    s_set_pres_secs = secs;
    if (s_events) xEventGroupSetBits(s_events, PRES_BIT);
}

void ha_client_notify_clear(void)
{
    if (s_events) xEventGroupSetBits(s_events, NOTIFY_BIT);
}

void ha_client_set_night(bool night, int night_pct, int timeout_s)
{
    s_set_night = night;
    s_set_night_pct = night_pct;
    s_set_scr_to = timeout_s;
    if (s_events) xEventGroupSetBits(s_events, NIGHT_BIT);
}

void ha_client_set_auto_rotate(bool on)
{
    s_set_autorot = on;
    if (s_events) xEventGroupSetBits(s_events, AUTOROT_BIT);
}

void ha_client_set_motion_wake(bool on)
{
    s_set_motion = on;
    if (s_events) xEventGroupSetBits(s_events, MOTION_BIT);
}

void ha_client_set_screensaver(bool on)
{
    s_set_saver = on;
    if (s_events) xEventGroupSetBits(s_events, SAVER_BIT);
}

void ha_client_set_brightness(float value)
{
    s_set_value = value;
    if (s_events) xEventGroupSetBits(s_events, SETBRI_BIT);
}

bool ha_client_pause(int timeout_ms)
{
    if (!s_events) return true;
    xEventGroupClearBits(s_events, RESUME_BIT);
    xEventGroupSetBits(s_events, PAUSE_BIT);
    /* the task finishes its current round first */
    return xEventGroupWaitBits(s_events, PAUSED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & PAUSED_BIT;
}

void ha_client_resume(void)
{
    if (!s_events) return;
    xEventGroupClearBits(s_events, PAUSE_BIT);
    xEventGroupSetBits(s_events, RESUME_BIT);
}

void ha_client_refresh(void)
{
    if (s_events) xEventGroupSetBits(s_events, REFRESH_BIT);
}

void ha_client_set_view(const char *path)
{
    taskENTER_CRITICAL(&s_view_lock);
    snprintf(s_view, sizeof(s_view), "%s", path ? path : "");
    taskEXIT_CRITICAL(&s_view_lock);
    if (s_events) xEventGroupSetBits(s_events, REFRESH_BIT);
}

void ha_client_hold_next(void) { s_hold_next = true; }

void ha_client_set_dashboard(const char *url_path, const char *view)
{
    taskENTER_CRITICAL(&s_view_lock);
    snprintf(s_dash, sizeof(s_dash), "%s", url_path ? url_path : "");
    snprintf(s_view, sizeof(s_view), "%s", view ? view : "");
    taskEXIT_CRITICAL(&s_view_lock);
    if (s_events) xEventGroupSetBits(s_events, REFRESH_BIT);
}

void ha_client_set_nav(const char *dash, const char *view, int page)
{
    taskENTER_CRITICAL(&s_view_lock);
    snprintf(s_wnav_dash, sizeof(s_wnav_dash), "%s", dash ? dash : "");
    snprintf(s_wnav_view, sizeof(s_wnav_view), "%s", view ? view : "");
    s_wnav_page = page;
    taskEXIT_CRITICAL(&s_view_lock);
    s_nav_writing = true;
    if (s_events) xEventGroupSetBits(s_events, NAV_BIT);
}

void ha_client_set_landscape(bool landscape)
{
    s_set_landscape = landscape;
    if (s_events) xEventGroupSetBits(s_events, ORIENT_BIT);
}
