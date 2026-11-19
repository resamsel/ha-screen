#include <stdarg.h>
static void add_error(const char *fmt, ...);
/* Wi-Fi station + Home Assistant REST client.
 * All network work runs in its own task so the LVGL UI never blocks. */
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <math.h>
#include <time.h>
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
#include "dash_fetch.h"
#include "layout_store.h"

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
static ha_layout_cb_t s_layout_cb;
void ha_client_set_layout_cb(ha_layout_cb_t cb) { s_layout_cb = cb; }
static ha_layout_t s_layout;              /* current layout, used only by the network task */
static uint32_t s_layout_hash;

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

static float read_entity(const layout_item_t *it, char text[24], char unit[8], char name[32]);
static void utf8_trim(char *s);

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

    for (int pg = 0; pg < s_layout.count; pg++) {
        if (s_layout.pages[pg].kind != LAYOUT_VALUES) continue;
        for (int i = 0; i < s_layout.pages[pg].count; i++) {
            const layout_item_t *it = &s_layout.pages[pg].items[i];
            char text[24], unit[8], name[32];
            read_entity(it, text, unit, name);
            if (s_sensor_cb) s_sensor_cb(pg, i, text, unit, name);
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
 * name to show (card name, else friendly_name, else the entity id). Returns the value, or NAN. */
static float read_entity(const layout_item_t *it, char text[24], char unit[8], char name[32])
{
    static resp_t r;
    char path[96];
    float v = NAN;
    strcpy(text, "--");
    unit[0] = 0;
    snprintf(name, 32, "%.31s", it->name[0] ? it->name : it->entity);
    snprintf(path, sizeof(path), "/api/states/%s", it->entity);
    if (ha_request(HTTP_METHOD_GET, path, NULL, &r) == 200) {
        if (!it->name[0] && json_str(r.buf, "\"friendly_name\":\"", name, 32)) utf8_trim(name);
        json_str(r.buf, "\"unit_of_measurement\":\"", unit, 8);
        if (json_num(r.buf, "\"state\":\"", &v))
            snprintf(text, 24, "%.*f", fabsf(v) < 100 ? 1 : 0, (double)v);   /* 1 decimal below 100 */
        else
            v = NAN;
    }
    return v;
}

/* Graph page: latest value every poll, then the history over the page's span */
static void fetch_graph(int pg)
{
    const layout_page_t *page = &s_layout.pages[pg];
    char latest[24], unit[8], name[32], path[256];
    read_entity(&page->items[0], latest, unit, name);
    bool zero_based = !strcmp(unit, "%");
    if (s_graph_cb) s_graph_cb(pg, latest, unit, name, NULL, 0, 0, page->hours, zero_based);

    if (!s_now) return;                   /* need the server time to place points */

    /* start time = now - span, as "2026-11-19T13:05:12+00:00" */
    time_t t0 = (time_t)(s_now - (int64_t)page->hours * 3600);
    struct tm g;
    gmtime_r(&t0, &g);
    char start[32], stop[32];
    strftime(start, sizeof(start), "%Y-%m-%dT%H:%M:%S%%2B00:00", &g);
    time_t t1 = (time_t)s_now;   /* without end_time HA returns only one day */
    gmtime_r(&t1, &g);
    strftime(stop, sizeof(stop), "%Y-%m-%dT%H:%M:%S%%2B00:00", &g);
    s_hist_hours = page->hours;
    memset(s_hist_has, 0, sizeof(s_hist_has));
    s_hist_len = 0;
    s_hist_mode = true;
    snprintf(path, sizeof(path), "/api/history/period/%s?end_time=%s&filter_entity_id=%s&minimal_response&no_attributes",
             start, stop, page->items[0].entity);
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
    ESP_LOGD(TAG, "history %dh: %d of %d buckets with data", page->hours, known, HA_GRAPH_POINTS);
    if (s_graph_cb) s_graph_cb(pg, NULL, NULL, NULL, pts, HA_GRAPH_POINTS, s_now, page->hours, zero_based);
}

/* An entity is usable unless HA says it doesn't exist or its state can never be a number.
 * unavailable/unknown keep their row ("--"); network errors keep it too. */
static bool entity_usable(const char *entity, void *ctx)
{
    (void)ctx;
    static resp_t r;
    char path[96], state[24];
    snprintf(path, sizeof(path), "/api/states/%s", entity);
    int st = ha_request(HTTP_METHOD_GET, path, NULL, &r);
    if (st == 404) { ESP_LOGW(TAG, "layout: %s does not exist, skipped", entity); return false; }
    if (st != 200 || !json_str(r.buf, "\"state\":\"", state, sizeof(state))) return true;
    char *end;
    strtof(state, &end);
    if (end != state || !strcmp(state, "unavailable") || !strcmp(state, "unknown")) return true;
    ESP_LOGW(TAG, "layout: %s is not numeric ('%s'), skipped", entity, state);
    return false;
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

static void use_layout(const ha_layout_t *l)
{
    s_layout = *l;
    s_layout_hash = layout_hash(l);
    if (s_layout_cb) s_layout_cb(&s_layout);
}

/* Re-read the dashboard; switch to it if it is usable and differs from the current one.
 * Failures keep the current layout and never reach the error box. */
static void refresh_layout(void)
{
    static ha_layout_t l;
    static uint32_t raw_hash;             /* parsed but unfiltered: skip the entity checks when unchanged */
    char *json = dash_fetch_config();
    if (!json) return;
    int n = layout_parse(json, HA_VIEW, &l);
    free(json);
    if (n && layout_hash(&l) == raw_hash && s_layout.count) return;
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
}

static void ha_task(void *arg)
{
    /* entity "switch.foo" -> domain "switch" */
    char path[96];
    const char *dot = strchr(HA_THEME_ENTITY, '.');
    snprintf(path, sizeof(path), "/api/services/%.*s/toggle", (int)(dot - HA_THEME_ENTITY), HA_THEME_ENTITY);
    const char *body = "{\"entity_id\":\"" HA_THEME_ENTITY "\"}";

    {   /* start with the cached layout, if any */
        static ha_layout_t cached;
        if (layout_store_load(&cached)) use_layout(&cached);
    }
    TickType_t layout_at = 0;
    bool layout_read = false;

    for (;;) {
        xEventGroupWaitBits(s_events, WIFI_UP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        /* wait for a tap, or poll the state every HA_POLL_INTERVAL_MS */
        EventBits_t b = xEventGroupWaitBits(s_events, TOGGLE_BIT | ORIENT_BIT | SETBRI_BIT | WAKE_BIT | REFRESH_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(HA_POLL_INTERVAL_MS));
        s_err[0] = 0;
        bool user = b & (TOGGLE_BIT | ORIENT_BIT | SETBRI_BIT | REFRESH_BIT);
        /* not while a tap waits: the dashboard check can take seconds */
        if (!user && (!layout_read || xTaskGetTickCount() - layout_at >= pdMS_TO_TICKS(HA_LAYOUT_INTERVAL_MS))) {
            refresh_layout();
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
        for (int pg = 0; pg < s_layout.count; pg++)
            if (s_layout.pages[pg].kind == LAYOUT_GRAPH) fetch_graph(pg);
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

    xTaskCreate(ha_task, "ha", 8192, NULL, 3, NULL);   /* also rebuilds the UI: 8 pages measured ~3.7 KB peak */
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
