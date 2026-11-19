#include <stdarg.h>
static void add_error(const char *fmt, ...);
/* Wi-Fi station + Home Assistant REST client.
 * All network work runs in its own task so the LVGL UI never blocks. */
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <math.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "ha_client.h"

#include "config.h"

#include "secrets_env.h"   /* WIFI_SSID, WIFI_PASS, HA_TOKEN from the build environment */

static const char *TAG = "ha";

#define WIFI_UP_BIT BIT0
#define TOGGLE_BIT  BIT1
#define ORIENT_BIT  BIT7
#define SETBRI_BIT  BIT2
#define WAKE_BIT    BIT3  /* Wi-Fi just came up: check HA right away */
#define REFRESH_BIT BIT4  /* user asked for a refresh */

static volatile float s_set_value;

static EventGroupHandle_t s_events;
static ha_state_cb_t s_state_cb;
static ha_landscape_cb_t s_land_cb;
void ha_client_set_landscape_cb(ha_landscape_cb_t cb) { s_land_cb = cb; }
static ha_brightness_cb_t s_brightness_cb;
static ha_done_cb_t s_done_cb;
static ha_sensor_cb_t s_sensor_cb;
static ha_graph_cb_t s_graph_cb;
static bool s_force_report;

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

/* ---- 24 h history, parsed while it streams in (the response can be large) ---- */
static bool s_hist_mode;                  /* route response body to the history parser */
static int64_t s_now;                     /* server time from the HTTP Date header (UTC s) */
static float s_hist_bucket[HA_GRAPH_POINTS];
static float s_hist_last[HA_GRAPH_POINTS];    /* value in effect at the end of the bucket */
static bool s_hist_has[HA_GRAPH_POINTS];
static char s_hist_line[384];
static int s_hist_len;
static int s_hist_depth;                  /* {} nesting: entries are the depth-1 objects */

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
    int64_t start = s_now - HA_GRAPH_HOURS * 3600;
    int64_t ts = parse_iso(lc + 16);
    int b = ts <= start ? 0 : (int)((ts - start) * HA_GRAPH_POINTS / (HA_GRAPH_HOURS * 3600));
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
        if (t) s_now = t;
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
    if (s_hist_mode) { memset(s_hist_has, 0, sizeof(s_hist_has)); s_hist_len = 0; s_hist_depth = 0; } /* fresh on retry */
    s_resp = resp;
    esp_http_client_set_url(s_http, url);
    esp_http_client_set_method(s_http, method);
    esp_http_client_set_post_field(s_http, body, body ? strlen(body) : 0);
    *err = esp_http_client_perform(s_http);
    s_resp = NULL;
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
            lv = v; lmn = mn; lmx = mx;
            s_force_report = false;
        }
    }

    /* Sensor pages (temperature, humidity): value + unit, or "--" when unavailable */
    static const char *entities[HA_SENSOR_PAGES][HA_ROW_COUNT] = { HA_TEMP_ENTITIES, HA_HUM_ENTITIES };
    static const int decimals[HA_SENSOR_PAGES] = { 1, 0 };
    for (int pg = 0; pg < HA_SENSOR_PAGES; pg++) {
        for (int i = 0; i < HA_ROW_COUNT; i++) {
            char path[96], text[24] = "--", unit[8] = "";
            snprintf(path, sizeof(path), "/api/states/%s", entities[pg][i]);
            if (ha_request(HTTP_METHOD_GET, path, NULL, &r) == 200) {
                float v;
                if (json_num(r.buf, "\"state\":\"", &v)) {
                    const char *u = strstr(r.buf, "\"unit_of_measurement\":\"");
                    if (u) {
                        u += 23;
                        int n = 0;
                        while (u[n] && u[n] != '"' && n < (int)sizeof(unit) - 1) { unit[n] = u[n]; n++; }
                        unit[n] = 0;
                    }
                    snprintf(text, sizeof(text), "%.*f", decimals[pg], (double)v);
                }
            }
            if (s_sensor_cb) s_sensor_cb(pg, i, text, unit);
        }
    }
    return st == 200;
}

/* Latest value and the 24 h history, every poll */
static void fetch_graph(int gi)
{
    static const char *entities[HA_GRAPH_COUNT] = HA_GRAPH_ENTITIES;
    static const int decimals[HA_GRAPH_COUNT] = HA_GRAPH_DECIMALS;
    static resp_t r;
    char path[192];
    char latest[24] = "--", unit[8] = "";
    snprintf(path, sizeof(path), "/api/states/%s", entities[gi]);
    if (ha_request(HTTP_METHOD_GET, path, NULL, &r) == 200) {
        float v;
        if (json_num(r.buf, "\"state\":\"", &v)) {
            const char *u = strstr(r.buf, "\"unit_of_measurement\":\"");
            if (u) {
                u += 23;
                int n = 0;
                while (u[n] && u[n] != '"' && n < (int)sizeof(unit) - 1) { unit[n] = u[n]; n++; }
            }
            snprintf(latest, sizeof(latest), "%.*f", decimals[gi], (double)v);
        }
    }
    if (s_graph_cb) s_graph_cb(gi, latest, unit, NULL, 0, 0);

    if (!s_now) return;                   /* need the server time to place points */

    memset(s_hist_has, 0, sizeof(s_hist_has));
    s_hist_len = 0;
    s_hist_mode = true;
    /* no start time in the path = HA's default of the last 24 h */
    snprintf(path, sizeof(path), "/api/history/period?filter_entity_id=%s&minimal_response&no_attributes", entities[gi]);
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
    ESP_LOGD(TAG, "history: %d of %d buckets with data", known, HA_GRAPH_POINTS);
    if (s_graph_cb) s_graph_cb(gi, NULL, NULL, pts, HA_GRAPH_POINTS, s_now);
}

static void ha_task(void *arg)
{
    /* entity "switch.foo" -> domain "switch" */
    char path[96];
    const char *dot = strchr(HA_THEME_ENTITY, '.');
    snprintf(path, sizeof(path), "/api/services/%.*s/toggle", (int)(dot - HA_THEME_ENTITY), HA_THEME_ENTITY);
    const char *body = "{\"entity_id\":\"" HA_THEME_ENTITY "\"}";

    for (;;) {
        xEventGroupWaitBits(s_events, WIFI_UP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        /* wait for a tap, or poll the state every HA_POLL_INTERVAL_MS */
        EventBits_t b = xEventGroupWaitBits(s_events, TOGGLE_BIT | ORIENT_BIT | SETBRI_BIT | WAKE_BIT | REFRESH_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(HA_POLL_INTERVAL_MS));
        s_err[0] = 0;
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
            ESP_LOGI(TAG, "toggle " HA_LANDSCAPE_ENTITY);
            int st = ha_request(HTTP_METHOD_POST, "/api/services/input_boolean/toggle",
                                "{\"entity_id\":\"" HA_LANDSCAPE_ENTITY "\"}", NULL);
            ok &= st >= 200 && st < 300;
        }
        if (b & TOGGLE_BIT) {
            ESP_LOGI(TAG, "toggle %s", HA_THEME_ENTITY);
            int st = ha_request(HTTP_METHOD_POST, path, body, NULL);
            ok &= st >= 200 && st < 300;
        }
        if (updated) {
            vTaskDelay(pdMS_TO_TICKS(300)); /* give HA a moment to update the state */
            s_force_report = true;         /* resync the UI even if nothing changed */
        }
        bool fetched = fetch_state();
        for (int gi = 0; gi < HA_GRAPH_COUNT; gi++) fetch_graph(gi);
        if (b & REFRESH_BIT) ok &= fetched;
        if (updated) {
            ESP_LOGI(TAG, "update %s", ok ? "succeeded" : "FAILED");
            if (s_done_cb) s_done_cb(ok);
        }
        if (s_err_cb) s_err_cb(s_err);
    }
}

void ha_client_start(ha_state_cb_t cb, ha_brightness_cb_t bcb, ha_done_cb_t dcb, ha_sensor_cb_t scb, ha_graph_cb_t gcb)
{
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

    xTaskCreate(ha_task, "ha", 8192, NULL, 3, NULL);
}

void ha_client_toggle(void)
{
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

void ha_client_toggle_landscape(void)
{
    if (s_events) xEventGroupSetBits(s_events, ORIENT_BIT);
}
