/* Wi-Fi and HA settings in NVS, and the setup portal (issue #15). See provision.h. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "config.h"
#include "provision.h"
#include "secrets_env.h"   /* optional build-time defaults */

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef HA_TOKEN
#define HA_TOKEN ""
#endif

static const char *TAG = "prov";
#define NS "prov"

static char s_ssid[33], s_pass[65], s_url[128], s_token[300], s_bearer[310];

static void load_key(nvs_handle_t h, bool open, const char *key, char *dst, size_t n, const char *def)
{
    size_t len = n;
    if (!open || nvs_get_str(h, key, dst, &len) != ESP_OK) snprintf(dst, n, "%s", def);
}

void prov_load(void)
{
    nvs_handle_t h;
    bool open = nvs_open(NS, NVS_READONLY, &h) == ESP_OK;
    load_key(h, open, "ssid", s_ssid, sizeof(s_ssid), WIFI_SSID);
    load_key(h, open, "pass", s_pass, sizeof(s_pass), WIFI_PASS);
    load_key(h, open, "url", s_url, sizeof(s_url), HA_URL_DEFAULT);
    load_key(h, open, "token", s_token, sizeof(s_token), HA_TOKEN);
    if (open) nvs_close(h);
    size_t l = strlen(s_url);
    while (l && s_url[l - 1] == '/') s_url[--l] = 0;
    snprintf(s_bearer, sizeof(s_bearer), "Bearer %s", s_token);
}

bool prov_ready(void) { return s_ssid[0] && s_token[0] && s_url[0]; }
const char *prov_ssid(void) { return s_ssid; }
const char *prov_pass(void) { return s_pass; }
const char *prov_url(void) { return s_url; }
const char *prov_token(void) { return s_token; }
const char *prov_bearer(void) { return s_bearer; }

static char s_reason[160];             /* why the portal opened ("" = asked for / first boot) */

void prov_request_portal(const char *reason)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "portal", 1);
    nvs_set_str(h, "why", reason ? reason : "");
    nvs_commit(h);
    nvs_close(h);
}

bool prov_portal_requested(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    if (nvs_get_u8(h, "portal", &v) == ESP_OK) {
        size_t n = sizeof(s_reason);
        if (nvs_get_str(h, "why", s_reason, &n) != ESP_OK) s_reason[0] = 0;
        nvs_erase_key(h, "portal");
        nvs_erase_key(h, "why");
        nvs_commit(h);
    }
    nvs_close(h);
    return v;
}

/* ---- Portal ------------------------------------------------------------------------ */

static int64_t s_last_activity;
static char s_board_id[24];           /* screen_a1b2c3, the default device name */

/* The device name (namespace "board", read by ha_client.c; an HA name helper overrides it) */
static void name_get(char *out, size_t n)
{
    nvs_handle_t h;
    size_t len = n;
    out[0] = 0;
    if (nvs_open("board", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "name", out, &len) != ESP_OK) out[0] = 0;
        nvs_close(h);
    }
    if (!out[0]) snprintf(out, n, "%s", s_board_id);
}

/* Captive DNS: answer every A query with the access point's address */
static void dns_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { ESP_LOGE(TAG, "dns bind failed"); vTaskDelete(NULL); }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || (buf[2] & 0x80)) continue;          /* too short, or a response */
        int q = 12;                                        /* skip the question name */
        while (q < n && buf[q]) q += buf[q] + 1;
        q += 5;                                            /* zero byte, type, class */
        if (q > n) continue;
        bool a_query = buf[q - 4] == 0 && buf[q - 3] == 1;
        buf[2] = 0x84; buf[3] = 0x00;                      /* response, authoritative */
        buf[6] = 0; buf[7] = a_query ? 1 : 0;              /* answers */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        n = q;
        if (a_query) {
            const uint8_t ans[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1 };
            memcpy(buf + n, ans, sizeof(ans));
            n += sizeof(ans);
        }
        sendto(s, buf, n, 0, (struct sockaddr *)&from, fl);
    }
}

static void html_escape(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    for (; *src && o + 7 < n; src++) {
        const char *r = *src == '<' ? "&lt;" : *src == '>' ? "&gt;" : *src == '&' ? "&amp;" : *src == '"' ? "&quot;" : NULL;
        if (r) { strcpy(dst + o, r); o += strlen(r); }
        else dst[o++] = *src;
    }
    dst[o] = 0;
}

/* An empty chunk would end the chunked response, so skip empty strings */
static void chunk(httpd_req_t *req, const char *s)
{
    if (s[0]) httpd_resp_sendstr_chunk(req, s);
}

static esp_err_t form_get(httpd_req_t *req)
{
    s_last_activity = esp_timer_get_time();
    char ssid[200], url[400], name[200], raw[32];
    name_get(raw, sizeof(raw));
    html_escape(name, sizeof(name), raw);
    html_escape(ssid, sizeof(ssid), s_ssid);
    html_escape(url, sizeof(url), s_url);
    httpd_resp_set_type(req, "text/html");
    chunk(req,
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>HA Screen setup</title><style>body{font-family:sans-serif;max-width:28em;margin:1em auto;padding:0 1em}"
        "label{display:block;margin-top:1em}input{width:100%;box-sizing:border-box;padding:.5em;font-size:1em}"
        "button{margin-top:1.5em;padding:.7em 2em;font-size:1em}small{color:#666}</style>"
        "<h2>HA Screen setup</h2>");
    if (s_reason[0]) {
        char why[400];
        html_escape(why, sizeof(why), s_reason);
        chunk(req, "<p style='background:#fde8e8;color:#8e0e0e;padding:.7em;border-radius:6px'>");
        chunk(req, why);
        chunk(req, "</p>");
    }
    chunk(req, "<form method=post action=/save>"
        "<label>Device name<input name=name autocapitalize=none autocorrect=off spellcheck=false maxlength=31 required value=\"");
    chunk(req, name);
    chunk(req, "\"></label><label>Wi-Fi name<input name=ssid autocapitalize=none autocorrect=off spellcheck=false maxlength=32 required value=\"");
    chunk(req, ssid);
    chunk(req, "\"></label><label>Wi-Fi password<input name=pass autocapitalize=none autocorrect=off spellcheck=false type=password maxlength=64 placeholder=\"");
    chunk(req, s_pass[0] ? "unchanged" : "");
    chunk(req, "\"></label><label>Home Assistant URL<input name=url autocapitalize=none autocorrect=off spellcheck=false type=url maxlength=127 required value=\"");
    chunk(req, url);
    chunk(req, "\"></label><label>Long-lived access token<input name=token autocapitalize=none autocorrect=off spellcheck=false maxlength=299 placeholder=\"");
    chunk(req, s_token[0] ? "unchanged" : "HA > profile > Security");
    chunk(req, "\"></label><small>Leave password or token empty to keep the stored one.</small>"
                                  "<br><button>Save and restart</button></form>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* Decodes the x-www-form-urlencoded value of `key` in `body` into dst; false if absent */
static bool form_value(const char *body, const char *key, char *dst, size_t n)
{
    size_t kl = strlen(key);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, kl) || p[kl] != '=') continue;
        p += kl + 1;
        size_t o = 0;
        while (*p && *p != '&' && o + 1 < n) {
            if (*p == '+') { dst[o++] = ' '; p++; }
            else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                char hx[3] = { p[1], p[2], 0 };
                dst[o++] = (char)strtol(hx, NULL, 16);
                p += 3;
            } else dst[o++] = *p++;
        }
        dst[o] = 0;
        return true;
    }
    return false;
}

static esp_err_t form_post(httpd_req_t *req)
{
    s_last_activity = esp_timer_get_time();
    if (req->content_len > 2048) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too long");
    char *body = calloc(1, req->content_len + 1);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) { free(body); return ESP_FAIL; }
        got += r;
    }
    char name[32] = "", ssid[33] = "", pass[65] = "", url[128] = "", token[300] = "";
    form_value(body, "name", name, sizeof(name));
    form_value(body, "ssid", ssid, sizeof(ssid));
    form_value(body, "pass", pass, sizeof(pass));
    form_value(body, "url", url, sizeof(url));
    form_value(body, "token", token, sizeof(token));
    free(body);
    size_t ul = strlen(url);
    while (ul && url[ul - 1] == '/') url[--ul] = 0;
    if (!name[0] || !ssid[0] || strncmp(url, "http", 4) || (!token[0] && !s_token[0]))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Device name, Wi-Fi name, URL (http:// or https://) and token are required");
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS error");
    /* a new network without a typed password is an open network */
    bool same_net = !strcmp(ssid, s_ssid);
    nvs_set_str(h, "ssid", ssid);
    if (pass[0] || !same_net) nvs_set_str(h, "pass", pass);
    nvs_set_str(h, "url", url);
    if (token[0]) nvs_set_str(h, "token", token);
    nvs_commit(h);
    nvs_close(h);
    if (nvs_open("board", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "name", name);
        nvs_set_u8(h, "push", 1);           /* ha_client.c writes it to the HA name helper */
        nvs_commit(h);
        nvs_close(h);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<!doctype html><meta name=viewport content='width=device-width'>"
                            "<p style='font-family:sans-serif'>Saved. The screen restarts and connects.</p>");
    ESP_LOGI(TAG, "settings saved, restarting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

/* Every other URL (OS connectivity checks) goes to the form, so the captive portal opens */
static esp_err_t redirect(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

void prov_portal_run(int idle_timeout_s, void (*shown)(const char *ap_name))
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);   /* same ID as the board in HA (ha_client.c) */
    snprintf(s_board_id, sizeof(s_board_id), "screen_%02x%02x%02x", mac[3], mac[4], mac[5]);
    const char *ap = s_board_id;
    if (shown) shown(ap);
    ESP_LOGI(TAG, "setup portal: join Wi-Fi %s, open http://192.168.4.1", ap);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    wifi_config_t wc = { .ap = { .channel = 6, .max_connection = 2, .authmode = WIFI_AUTH_OPEN } };
    strcpy((char *)wc.ap.ssid, ap);
    wc.ap.ssid_len = strlen(ap);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 6144;
    httpd_handle_t srv;
    ESP_ERROR_CHECK(httpd_start(&srv, &hc));
    httpd_uri_t get = { .uri = "/", .method = HTTP_GET, .handler = form_get };
    httpd_uri_t post = { .uri = "/save", .method = HTTP_POST, .handler = form_post };
    httpd_register_uri_handler(srv, &get);
    httpd_register_uri_handler(srv, &post);
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, redirect);

    s_last_activity = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        wifi_sta_list_t sta;
        if (esp_wifi_ap_get_sta_list(&sta) == ESP_OK && sta.num) s_last_activity = esp_timer_get_time();
        if (prov_ready() && esp_timer_get_time() - s_last_activity > (int64_t)idle_timeout_s * 1000000) {
            ESP_LOGI(TAG, "setup portal idle, restarting with the stored settings");
            esp_restart();
        }
    }
}
