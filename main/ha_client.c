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
#include "ha_client.h"
#include "dash_fetch.h"
#include "icon_rule.h"
#include "cache_store.h"
#include "layout_store.h"
#include "names.h"
#include "rom/tjpgd.h"
#include "miniz.h"

#include "config.h"

#include "secrets_env.h"   /* WIFI_SSID, WIFI_PASS, HA_TOKEN from the build environment */

static const char *TAG = "ha";

#define WIFI_UP_BIT BIT0
#define TOGGLE_BIT  BIT1
#define ORIENT_BIT  BIT7
#define SETBRI_BIT  BIT2
#define WAKE_BIT    BIT3  /* Wi-Fi just came up: check HA right away */
#define REFRESH_BIT BIT4  /* user asked for a refresh */
#define PRESS_BIT   BIT5  /* a dashboard button was tapped */
#define PIC_BIT     BIT6  /* another page is shown: fetch its pictures now */

static volatile float s_set_value;
static volatile bool s_set_dark, s_set_landscape;   /* values for TOGGLE_BIT / ORIENT_BIT */

static EventGroupHandle_t s_events;
static ha_state_cb_t s_state_cb;
static ha_landscape_cb_t s_land_cb;
void ha_client_set_landscape_cb(ha_landscape_cb_t cb) { s_land_cb = cb; }
static ha_brightness_cb_t s_brightness_cb;
static ha_done_cb_t s_done_cb;
static ha_sensor_cb_t s_sensor_cb;
static ha_graph_cb_t s_graph_cb;
static bool s_force_report;
static ha_layout_cb_t s_layout_cb;
void ha_client_set_layout_cb(ha_layout_cb_t cb) { s_layout_cb = cb; }
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
static ha_weather_cb_t s_weather_cb;
void ha_client_set_weather_cb(ha_weather_cb_t cb) { s_weather_cb = cb; }
static ha_forecast_cb_t s_forecast_cb;
void ha_client_set_forecast_cb(ha_forecast_cb_t cb) { s_forecast_cb = cb; }
static portMUX_TYPE s_pic_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_pic_page = -1;               /* page on screen, and its picture block sizes */
static uint16_t s_pic_w[LAYOUT_MAX_ROWS], s_pic_h[LAYOUT_MAX_ROWS];
static uint32_t s_layout_hash;
static ha_views_cb_t s_views_cb;
void ha_client_set_views_cb(ha_views_cb_t cb) { s_views_cb = cb; }
static portMUX_TYPE s_view_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_view[32];                   /* view path chosen in the config panel, "" = first */

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
        xEventGroupSetBits(s_events, WIFI_UP_BIT | WAKE_BIT);
    }
}

/* Small response buffer: we only need the "state" field */
typedef struct { char buf[2048]; int len; } resp_t;

/* One HTTPS connection is kept open and reused for every request, so the TLS
 * handshake (the slow part) happens only once instead of per request. */
static esp_http_client_handle_t s_http;
static resp_t *s_resp;
/* Forecast responses are parsed while they stream in (they can be large): each entry
 * object after "forecast":[ is collected and handed to fc_entry */
static bool s_fc_mode;
static char s_fc_obj[320];
static int s_fc_len, s_fc_state;          /* 0 = before the list, 1 = in it, 2 = in an entry, 3 = done */
static void fc_entry(char *obj);
static ha_forecast_t s_fc[HA_FORECAST_MAX];
static int s_fc_n;

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

/* One history entry: {"state":"0.0","last_changed":"..."} */
static void hist_point(const char *obj)
{
    const char *st = strstr(obj, "\"state\":\"");
    const char *lc = strstr(obj, "\"last_changed\":\"");
    if (!st || !lc || !s_now) return;
    char *end;
    float v = strtof(st + 9, &end);
    if (end == st + 9) return;            /* "unavailable" etc. */
    int64_t start = s_now - (int64_t)s_hist_hours * 3600;
    int64_t ts = parse_iso(lc + 16);
    int b = ts <= start ? 0 : (int)((ts - start) * HA_GRAPH_POINTS / ((int64_t)s_hist_hours * 3600));
    if (b < 0 || b >= HA_GRAPH_POINTS) return;
    if (!s_hist_has[b] || v > s_hist_bucket[b]) s_hist_bucket[b] = v;  /* keep spikes */
    s_hist_last[b] = v;                   /* history is in time order: last one wins */
    s_hist_has[b] = true;
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
        esp_http_client_set_header(s_http, "Authorization", "Bearer " HA_TOKEN);
        esp_http_client_set_header(s_http, "Content-Type", "application/json");
    }
    if (resp) { resp->len = 0; resp->buf[0] = 0; }
    if (s_fc_mode) { s_fc_state = 0; s_fc_len = 0; s_fc_n = 0; }   /* fresh on retry */
    if (s_hist_mode) { memset(s_hist_has, 0, sizeof(s_hist_has)); s_hist_len = 0; s_hist_depth = 0; } /* fresh on retry */
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
static int ha_request(esp_http_client_method_t method, const char *path, const char *body, resp_t *resp)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", HA_URL, path);
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
    if (status < 0) {
        ESP_LOGE(TAG, "%s failed: %s", path, esp_err_to_name(err));
        add_error("%s: %s", path, esp_err_to_name(err));
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
enum { REC_VALUE, REC_GLATEST, REC_GPOINTS, REC_BUTTON, REC_WEATHER, REC_ICON, REC_FORECAST, REC_TYPES };
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
    if (s_graph_cb) s_graph_cb(pg, row, latest, unit, name, pts, n, end, hours, zero_based);
}

static void emit_button(int pg, int row, ha_btn_state_t st, const char *name)
{
    uint8_t b[40];
    b[0] = st;
    const char *a[] = { name };
    rec_put(REC_BUTTON, pg, row, b, 1 + pack_strs(b + 1, sizeof(b) - 1, a, 1));
    if (s_button_cb) s_button_cb(pg, row, st, name);
}

static void emit_weather(int pg, int row, const char *cond, const char *temp, const char *unit,
                         const char *hum, const char *name)
{
    uint8_t b[96];
    const char *a[] = { cond, temp, unit, hum, name };
    rec_put(REC_WEATHER, pg, row, b, pack_strs(b, sizeof(b), a, 5));
    if (s_weather_cb) s_weather_cb(pg, row, cond, temp, unit, hum, name);
}

/* record: u8 hourly, u8 n, n x ha_forecast_t */
static void emit_forecast(int pg, int row, const ha_forecast_t *f, int n, bool hourly)
{
    uint8_t b[2 + HA_FORECAST_MAX * sizeof(ha_forecast_t)];
    if (n < 0 || n > HA_FORECAST_MAX) return;
    b[0] = hourly; b[1] = n;
    memcpy(b + 2, f, n * sizeof(ha_forecast_t));
    rec_put(REC_FORECAST, pg, row, b, 2 + n * sizeof(ha_forecast_t));
    if (s_forecast_cb) s_forecast_cb(pg, row, f, n, hourly);
}

static void emit_icon(int pg, int row, const char *icon)
{
    uint8_t b[52];
    const char *a[] = { icon };
    rec_put(REC_ICON, pg, row, b, pack_strs(b, sizeof(b), a, 1));
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
static void report_icon(int pg, int row, const char *json);
static void utf8_trim(char *s);
static void fetch_weather(int pg, int row);
static void fetch_forecast(int pg, int row);
static void fetch_button(int pg, int row);

/* A tap/toggle is waiting: background reads (graph history, pictures, the rest of a poll)
 * stop so it reaches HA at once; they are picked up again on the next poll */
static bool user_waiting(void)
{
    return xEventGroupGetBits(s_events) & (TOGGLE_BIT | ORIENT_BIT | SETBRI_BIT | PRESS_BIT);
}

static bool fetch_state(void)
{
    static resp_t r;
    int st = ha_request(HTTP_METHOD_GET, "/api/states/" HA_THEME_ENTITY, NULL, &r);
    ha_state_t s = HA_STATE_UNKNOWN;
    if (st == 200) {
        if (strstr(r.buf, "\"state\":\"on\"")) s = HA_STATE_ON;
        else if (strstr(r.buf, "\"state\":\"off\"")) s = HA_STATE_OFF;
    } else {
        s = HA_STATE_ERROR;
    }
    if ((s == HA_STATE_ON || s == HA_STATE_OFF) && s != s_rec_theme) { s_rec_theme = s; s_rec_dirty = true; }
    if (s_state_cb) s_state_cb(s);

    if (ha_request(HTTP_METHOD_GET, "/api/states/" HA_LANDSCAPE_ENTITY, NULL, &r) == 200) {
        bool on = strstr(r.buf, "\"state\":\"on\"") != NULL;
        bool off = strstr(r.buf, "\"state\":\"off\"") != NULL;
        if ((on || off) && s_land_cb) s_land_cb(on);
    }

    /* Brightness: input_number state like "42.0" plus its min/max attributes */
    if (ha_request(HTTP_METHOD_GET, "/api/states/" HA_BRIGHTNESS_ENTITY, NULL, &r) == 200) {
        float v, mn = 0, mx = 100;
        if (json_num(r.buf, "\"state\":\"", &v)) {
            json_num(r.buf, "\"min\":", &mn);
            json_num(r.buf, "\"max\":", &mx);
            static float lv = -1e9f, lmn, lmx;
            if ((s_force_report || v != lv || mn != lmn || mx != lmx) && s_brightness_cb) s_brightness_cb(v, mn, mx);
            if (!s_rec_bri_has || s_rec_bri[0] != v || s_rec_bri[1] != mn || s_rec_bri[2] != mx) {
                s_rec_bri[0] = v; s_rec_bri[1] = mn; s_rec_bri[2] = mx;
                s_rec_bri_has = s_rec_dirty = true;
            }
            lv = v; lmn = mn; lmx = mx;
            s_force_report = false;
        }
    }

    for (int pg = 0; pg < s_layout.count && !user_waiting(); pg++) {
        for (int i = 0; i < s_layout.pages[pg].count && !user_waiting(); i++) {
            const layout_item_t *it = &s_layout.pages[pg].items[i];
            if (it->kind == LAYOUT_BUTTONS) { fetch_button(pg, i); continue; }
            if (it->kind == LAYOUT_WEATHER) { fetch_weather(pg, i); continue; }
            if (it->kind != LAYOUT_VALUES && it->kind != LAYOUT_GAUGE && it->kind != LAYOUT_GLANCE) continue;   /* graphs: fetch_graph; clocks: no data */
            char text[24], unit[8], name[32];
            bool got;
            read_entity(pg, i, text, unit, name, &got);
            if (got) emit_sensor(pg, i, text, unit, name);   /* failed: keep the last value */
        }
    }
    return st == 200;
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
    static resp_t r;
    char path[96];
    float v = NAN;
    strcpy(text, "--");
    unit[0] = 0;
    snprintf(name, 32, "%.31s", it->name[0] ? it->name : it->entity);
    snprintf(path, sizeof(path), "/api/states/%s", it->entity);
    *got = ha_request(HTTP_METHOD_GET, path, NULL, &r) == 200;
    if (*got) {
        report_icon(pg, row, r.buf);
        if (!it->name[0] && json_str(r.buf, "\"friendly_name\":\"", name, 32)) utf8_trim(name);
        json_str(r.buf, "\"unit_of_measurement\":\"", unit, 8);
        char st[48];
        if (json_num(r.buf, "\"state\":\"", &v)) {
            snprintf(text, 24, "%.*f", fabsf(v) < 100 ? 1 : 0, (double)v);   /* 1 decimal below 100 */
        } else {
            v = NAN;
            /* text states ("not_charging" -> "Not charging"); no unit for them */
            if (json_str(r.buf, "\"state\":\"", st, sizeof(st)) && strcmp(st, "unavailable") && strcmp(st, "unknown")) {
                state_text(st, text, 24);
                unit[0] = 0;
            }
        }
    }
    return v;
}

/* Graph page: latest value every poll, then the history over the page's span */
static void fetch_graph(int pg, int row)
{
    const layout_item_t *it = &s_layout.pages[pg].items[row];
    char latest[24], unit[8], name[32], path[256];
    bool got;
    read_entity(pg, row, latest, unit, name, &got);
    bool zero_based = !strcmp(unit, "%");
    if (got) emit_graph(pg, row, latest, unit, name, NULL, 0, 0, it->hours, zero_based);

    if (!s_now) return;                   /* need the server time to place points */

    /* start time = now - span, as "2026-11-19T13:05:12+00:00" */
    time_t t0 = (time_t)(s_now - (int64_t)it->hours * 3600);
    struct tm g;
    gmtime_r(&t0, &g);
    char start[32], stop[32];
    strftime(start, sizeof(start), "%Y-%m-%dT%H:%M:%S%%2B00:00", &g);
    time_t t1 = (time_t)s_now;   /* without end_time HA returns only one day */
    gmtime_r(&t1, &g);
    strftime(stop, sizeof(stop), "%Y-%m-%dT%H:%M:%S%%2B00:00", &g);
    s_hist_hours = it->hours;
    memset(s_hist_has, 0, sizeof(s_hist_has));
    s_hist_len = 0;
    s_hist_mode = true;
    snprintf(path, sizeof(path), "/api/history/period/%s?end_time=%s&filter_entity_id=%s&minimal_response&no_attributes",
             start, stop, it->entity);
    int st = ha_request(HTTP_METHOD_GET, path, NULL, NULL);
    s_hist_mode = false;
    if (st != 200) return;

    /* a state holds until it changes: carry values forward into empty buckets */
    static float pts[HA_GRAPH_POINTS];
    float carry = NAN;
    int known = 0;
    for (int i = 0; i < HA_GRAPH_POINTS; i++) {
        /* a bucket shows its peak; the gap after it continues with the value the
         * sensor ended on, not the peak (a 20 % spike that drops back to 0 within
         * the same bucket must not draw 20 % for hours) */
        if (s_hist_has[i]) { pts[i] = s_hist_bucket[i]; carry = s_hist_last[i]; known++; }
        else pts[i] = carry;
    }
    ESP_LOGD(TAG, "history %dh: %d of %d buckets with data", it->hours, known, HA_GRAPH_POINTS);
    emit_graph(pg, row, NULL, NULL, NULL, pts, HA_GRAPH_POINTS, s_now, it->hours, zero_based);
}

static bool is_domain(const char *entity, const char *domain)
{
    size_t n = strlen(domain);
    return !strncmp(entity, domain, n) && entity[n] == '.';
}

/* Button state and display name */
static void fetch_button(int pg, int row)
{
    static resp_t r;
    const layout_item_t *it = &s_layout.pages[pg].items[row];
    char path[96], state[24] = "", name[32];
    snprintf(name, sizeof(name), "%.31s", it->name[0] ? it->name : it->entity);
    snprintf(path, sizeof(path), "/api/states/%s", it->entity);
    ha_btn_state_t bs = HA_BTN_UNAVAILABLE;
    if (ha_request(HTTP_METHOD_GET, path, NULL, &r) != 200) return;   /* keep the last state */
    {
        report_icon(pg, row, r.buf);
        if (!it->name[0] && json_str(r.buf, "\"friendly_name\":\"", name, sizeof(name))) utf8_trim(name);
        json_str(r.buf, "\"state\":\"", state, sizeof(state));
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
    int st = ha_request(HTTP_METHOD_POST, svc, body, NULL);
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
    static resp_t r;
    char path[96], state[24];
    snprintf(path, sizeof(path), "/api/states/%s", entity);
    int st = ha_request(HTTP_METHOD_GET, path, NULL, &r);
    if (st == 404) { ESP_LOGW(TAG, "layout: %s does not exist, skipped", entity); return false; }
    if (st != 200 || !json_str(r.buf, "\"state\":\"", state, sizeof(state))) return true;
    if (!strcmp(state, "unavailable") || !strcmp(state, "unknown")) return true;
    if (kind == LAYOUT_BUTTONS) {
        if (is_domain(entity, "script") || is_domain(entity, "scene") ||
            is_domain(entity, "button") || is_domain(entity, "input_button") ||   /* state = last press time */
            !strcmp(state, "on") || !strcmp(state, "off")) return true;
        ESP_LOGW(TAG, "layout: %s can't be switched ('%s'), skipped", entity, state);
        return false;
    }
    if (kind == LAYOUT_VALUES || kind == LAYOUT_GLANCE) return true;   /* text states are shown as text */
    if (kind == LAYOUT_WEATHER) {
        if (is_domain(entity, "weather")) return true;
        ESP_LOGW(TAG, "layout: %s is not a weather entity, skipped", entity);
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
    static resp_t r;
    const layout_item_t *it = &s_layout.pages[pg].items[row];
    char path[96], cond[24] = "", temp[12] = "", hum[8] = "", unit[8] = "", name[32];
    snprintf(name, sizeof(name), "%.31s", it->name[0] ? it->name : it->entity);
    snprintf(path, sizeof(path), "/api/states/%s", it->entity);
    if (ha_request(HTTP_METHOD_GET, path, NULL, &r) != 200) return;   /* keep the last data */
    {
        if (!it->name[0] && json_str(r.buf, "\"friendly_name\":\"", name, sizeof(name))) utf8_trim(name);
        json_str(r.buf, "\"state\":\"", cond, sizeof(cond));
        if (!strcmp(cond, "unavailable") || !strcmp(cond, "unknown")) cond[0] = 0;
        json_numtext(r.buf, "\"temperature\":", temp, sizeof(temp));
        json_numtext(r.buf, "\"humidity\":", hum, sizeof(hum));
        json_str(r.buf, "\"temperature_unit\":\"", unit, sizeof(unit));
    }
    emit_weather(pg, row, cond, temp, unit, hum, name);
    if (it->flags & LAYOUT_WX_FORECAST) fetch_forecast(pg, row);
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
    static resp_t r;
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
            if (ha_request(HTTP_METHOD_POST, "/api/template", body, &r) != 200) continue;
            snprintf(it->name, sizeof(it->name), "%.31s", r.buf);
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
    return (it->kind == LAYOUT_VALUES || it->kind == LAYOUT_GRAPH || it->kind == LAYOUT_BUTTONS || it->kind == LAYOUT_GLANCE) &&
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

static void use_layout(const ha_layout_t *l)
{
    rec_lock();
    if (layout_hash(l) != s_layout_hash) rec_clear();   /* the rows mean something else now */
    s_layout = *l;
    s_layout_hash = layout_hash(l);
    rec_unlock();
    if (s_layout_cb) s_layout_cb(&s_layout);
    load_icon_rules();
}

/* Re-read the dashboard; switch to it if it is usable and differs from the current one.
 * Failures keep the current layout and never reach the error box. */
/* force: re-check entities and names even if the dashboard itself is unchanged */
/* one scratch layout for the boot cache and every dashboard re-read (~12 KB of RAM) */
static ha_layout_t s_scratch;

static void refresh_layout(bool force)
{
    ha_layout_t *lp = &s_scratch;
#define l (*lp)
    static uint32_t raw_hash;             /* parsed but unfiltered: skip the entity checks when unchanged */
    if (s_http) {                         /* two TLS sessions at once don't fit in RAM */
        esp_http_client_cleanup(s_http);
        s_http = NULL;
    }
    char *json = dash_fetch_config();
    if (!json) return;
    static layout_view_t views[LAYOUT_MAX_VIEWS];
    char want[32], chosen[32];
    int nv;
    taskENTER_CRITICAL(&s_view_lock);
    memcpy(want, s_view, sizeof(want));
    taskEXIT_CRITICAL(&s_view_lock);
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
    if (layout_hash(&l) == s_layout_hash) return;
    ESP_LOGI(TAG, "new layout: %d pages", n);
    layout_store_save(&l);
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
static uint8_t s_pic_win[TINFL_LZ_DICT_SIZE];
static tinfl_decompressor s_pic_inf;


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
    pic_dec_t d = { .win = s_pic_win, .inf = &s_pic_inf, .fit = fit };
    d.http = esp_http_client_init(&cfg);
    if (!d.http) return NULL;
    if (auth) esp_http_client_set_header(d.http, "Authorization", "Bearer " HA_TOKEN);
    const char *err = NULL, *path = auth ? url + strlen(HA_URL) : url;
    int st = -1;
    if (esp_http_client_open(d.http, 0) != ESP_OK || esp_http_client_fetch_headers(d.http) < 0) err = "connection failed";
    else if ((st = esp_http_client_get_status_code(d.http)) != 200) err = "HTTP error";
    else if ((d.head_len = pic_read(&d, d.head, 8)) != 8) err = "empty response";
    else if (!memcmp(d.head, "\x89PNG", 4)) err = png_decode(&d, bw, bh, pg, row);
    else if (d.head[0] == 0xFF && d.head[1] == 0xD8) err = jpg_decode(&d, bw, bh, pg, row);
    else err = "not a JPEG or PNG";
    esp_http_client_close(d.http);
    esp_http_client_cleanup(d.http);
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
        static resp_t r;
        static char url[LAYOUT_IMAGE_LEN + 128];
        char path[128], name[32], key[LAYOUT_IMAGE_LEN] = "";
        snprintf(name, sizeof(name), "%.31s", it->name[0] ? it->name : it->entity);
        const char *src = it->image ? s_layout.images[it->image - 1] : it->entity;
        bool camera = is_domain(src, "camera"), entity = camera || is_domain(src, "image");
        snprintf(path, sizeof(path), "/api/states/%s", it->entity);
        if (it->entity[0] && ha_request(HTTP_METHOD_GET, path, NULL, &r) == 200) {   /* picture-glance: no entity */
            if (!it->name[0] && json_str(r.buf, "\"friendly_name\":\"", name, sizeof(name))) utf8_trim(name);
            if (!it->image) json_str(r.buf, "\"state\":\"", key, sizeof(key));
        }
        if (it->image && !entity) snprintf(key, sizeof(key), "%s", src);
        if (!camera && key[0] && !strcmp(key, s_pic_state[i])) continue;   /* same image */
        bool auth = true;
        if (camera) snprintf(url, sizeof(url), "%s/api/camera_proxy/%s?width=%d&height=%d", HA_URL, src, w[i], h[i]);
        else if (entity) snprintf(url, sizeof(url), "%s/api/image_proxy/%s", HA_URL, src);
        else if (strstr(src, "://")) { snprintf(url, sizeof(url), "%s", src); auth = !strncmp(src, HA_URL "/", strlen(HA_URL) + 1); }
        else snprintf(url, sizeof(url), "%s%s%s", HA_URL, src[0] == '/' ? "" : "/", src);
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
    if (s_events) xEventGroupSetBits(s_events, PIC_BIT);
}

static void ha_task(void *arg)
{
    /* entity "switch.foo" -> domain "switch" */
    char path[96];
    const char *dot = strchr(HA_THEME_ENTITY, '.');
    const char *body = "{\"entity_id\":\"" HA_THEME_ENTITY "\"}";

    {   /* start with the cached layout, if any */
        if (layout_store_load(&s_scratch)) {
            use_layout(&s_scratch);
            cache_replay();               /* the last data, until HA answers */
        }
    }
    TickType_t layout_at = 0, pic_at = 0;
    bool layout_read = false;

    for (;;) {
        xEventGroupWaitBits(s_events, WIFI_UP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        /* wait for a tap, or poll the state every HA_POLL_INTERVAL_MS */
        EventBits_t b = xEventGroupWaitBits(s_events, TOGGLE_BIT | ORIENT_BIT | SETBRI_BIT | WAKE_BIT | REFRESH_BIT | PRESS_BIT | PIC_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(HA_POLL_INTERVAL_MS));
        s_err[0] = 0;
        if (b == PIC_BIT) {               /* only a page switch: just its pictures */
            fetch_pictures(true);
            pic_at = xTaskGetTickCount();
            continue;
        }
        if (s_busy_cb) s_busy_cb(true);
        bool user = b & (TOGGLE_BIT | ORIENT_BIT | SETBRI_BIT | PRESS_BIT);
        bool refresh = b & REFRESH_BIT;   /* the refresh button also re-reads the dashboard */
        /* not while a tap waits: the dashboard check can take seconds */
        if (refresh || (!user && (!layout_read || xTaskGetTickCount() - layout_at >= pdMS_TO_TICKS(HA_LAYOUT_INTERVAL_MS)))) {
            refresh_layout(refresh);
            layout_at = xTaskGetTickCount();
            layout_read = true;
        }
        bool updated = b & (TOGGLE_BIT | ORIENT_BIT | SETBRI_BIT | REFRESH_BIT);
        bool ok = true;
        if (b & SETBRI_BIT) {
            char sb[128];
            snprintf(sb, sizeof(sb), "{\"entity_id\":\"" HA_BRIGHTNESS_ENTITY "\",\"value\":%.1f}", (double)s_set_value);
            ESP_LOGI(TAG, "set brightness %.1f", (double)s_set_value);
            int st = ha_request(HTTP_METHOD_POST, "/api/services/input_number/set_value", sb, NULL);
            ok &= st >= 200 && st < 300;
        }
        if (b & ORIENT_BIT) {
            bool on = s_set_landscape;
            ESP_LOGI(TAG, HA_LANDSCAPE_ENTITY " %s", on ? "on" : "off");
            int st = ha_request(HTTP_METHOD_POST, on ? "/api/services/input_boolean/turn_on" : "/api/services/input_boolean/turn_off",
                                "{\"entity_id\":\"" HA_LANDSCAPE_ENTITY "\"}", NULL);
            ok &= st >= 200 && st < 300;
        }
        if (b & TOGGLE_BIT) {
            bool on = s_set_dark;
            snprintf(path, sizeof(path), "/api/services/%.*s/turn_%s", (int)(dot - HA_THEME_ENTITY), HA_THEME_ENTITY, on ? "on" : "off");
            ESP_LOGI(TAG, "%s %s", HA_THEME_ENTITY, on ? "on" : "off");
            int st = ha_request(HTTP_METHOD_POST, path, body, NULL);
            ok &= st >= 200 && st < 300;
        }
        if (b & PRESS_BIT) {
            int pg = s_press_page, row = s_press_row;
            bool pok = press_button(pg, row);
            if (s_press_cb) s_press_cb(pg, row, pok);
            vTaskDelay(pdMS_TO_TICKS(300)); /* let HA switch before the state is re-read */
        }
        if (updated) {
            vTaskDelay(pdMS_TO_TICKS(300)); /* give HA a moment to update the state */
            s_force_report = true;         /* resync the UI even if nothing changed */
        }
        if (s_icon_rules_pending) load_icon_rules();
        bool fetched = fetch_state();
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
        if (s_err_cb) s_err_cb(s_err);
        if (s_busy_cb) s_busy_cb(false);
    }
}

/* Re-send the last data of every card, the theme and the brightness through the callbacks */
void ha_client_replay(void)
{
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

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    xTaskCreate(ha_task, "ha", 8192, NULL, 3, NULL);   /* also rebuilds the UI: 8 pages measured ~3.7 KB peak */
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

void ha_client_set_brightness(float value)
{
    s_set_value = value;
    if (s_events) xEventGroupSetBits(s_events, SETBRI_BIT);
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

void ha_client_set_landscape(bool landscape)
{
    s_set_landscape = landscape;
    if (s_events) xEventGroupSetBits(s_events, ORIENT_BIT);
}
