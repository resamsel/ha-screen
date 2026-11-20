#include "dash_fetch.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "config.h"
#include "secrets_env.h"

#define MAX_LEN     (32 * 1024)
#define TIMEOUT_MS  10000
#define AUTH_REQ_BIT BIT0
#define AUTH_OK_BIT  BIT1
#define DONE_BIT     BIT2
#define FAIL_BIT     BIT3

static const char *TAG = "dash";

typedef struct {
    EventGroupHandle_t ev;
    char *buf;      /* message being assembled */
    int len;
    int want;       /* id of the request being waited for */
    char *result;   /* its complete result message */
} ctx_t;

struct dash_ws {
    esp_websocket_client_handle_t ws;
    ctx_t c;
    int id;
};

static int msg_id(const char *msg)
{
    const char *p = strstr(msg, "\"id\":");
    return p ? atoi(p + 5) : 0;
}

static void on_message(ctx_t *c, char *msg)
{
    if (strstr(msg, "\"type\":\"auth_required\"")) xEventGroupSetBits(c->ev, AUTH_REQ_BIT);
    else if (strstr(msg, "\"type\":\"auth_ok\"")) xEventGroupSetBits(c->ev, AUTH_OK_BIT);
    else if (strstr(msg, "\"type\":\"auth_invalid\"")) { ESP_LOGW(TAG, "auth rejected"); xEventGroupSetBits(c->ev, FAIL_BIT); }
    else if (!c->result && c->want && msg_id(msg) == c->want) {
        c->result = msg;
        xEventGroupSetBits(c->ev, DONE_BIT);
        return;
    }
    free(msg);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ctx_t *c = arg;
    esp_websocket_event_data_t *d = data;
    if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR) {
        xEventGroupSetBits(c->ev, FAIL_BIT);
        return;
    }
    if (id != WEBSOCKET_EVENT_DATA || d->op_code > 2) return;  /* text/continuation only */
    /* a message can arrive in pieces: payload_offset/payload_len describe the whole */
    if (d->payload_offset == 0) {
        free(c->buf);
        c->len = 0;
        c->buf = d->payload_len < MAX_LEN ? malloc(d->payload_len + 1) : NULL;
        if (!c->buf) { ESP_LOGW(TAG, "message too large (%d)", d->payload_len); xEventGroupSetBits(c->ev, FAIL_BIT); return; }
    }
    if (!c->buf || c->len + d->data_len > d->payload_len) return;
    memcpy(c->buf + c->len, d->data_ptr, d->data_len);
    c->len += d->data_len;
    if (c->len == d->payload_len) {
        c->buf[c->len] = 0;
        char *msg = c->buf;
        c->buf = NULL;
        on_message(c, msg);
    }
}

static bool wait(ctx_t *c, EventBits_t bit, TickType_t deadline)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t left = deadline > now ? deadline - now : 0;
    EventBits_t b = xEventGroupWaitBits(c->ev, bit | FAIL_BIT, pdFALSE, pdFALSE, left);
    return (b & bit) && !(b & FAIL_BIT);
}

dash_ws_t *dash_ws_open(void)
{
    char uri[160];
    const char *host = strstr(HA_URL, "://");
    snprintf(uri, sizeof(uri), "%s%s/api/websocket", strncmp(HA_URL, "https", 5) ? "ws" : "wss", host ? host : "");
    dash_ws_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->c.ev = xEventGroupCreate();
    if (!d->c.ev) { free(d); return NULL; }
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .network_timeout_ms = 5000,
        .disable_auto_reconnect = true,
    };
    d->ws = esp_websocket_client_init(&cfg);
    if (!d->ws) { dash_ws_close(d); return NULL; }
    esp_websocket_register_events(d->ws, WEBSOCKET_EVENT_ANY, ws_event, &d->c);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TIMEOUT_MS);
    if (esp_websocket_client_start(d->ws) == ESP_OK && wait(&d->c, AUTH_REQ_BIT, deadline)) {
        const char *auth = "{\"type\":\"auth\",\"access_token\":\"" HA_TOKEN "\"}";
        esp_websocket_client_send_text(d->ws, auth, strlen(auth), pdMS_TO_TICKS(TIMEOUT_MS));
        if (wait(&d->c, AUTH_OK_BIT, deadline)) return d;
    }
    ESP_LOGW(TAG, "websocket connect failed");
    dash_ws_close(d);
    return NULL;
}

char *dash_ws_call(dash_ws_t *d, const char *body)
{
    if (!d) return NULL;
    char *req = malloc(strlen(body) + 24);
    if (!req) return NULL;
    d->c.want = ++d->id;
    xEventGroupClearBits(d->c.ev, DONE_BIT);
    sprintf(req, "{\"id\":%d,%s}", d->id, body);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TIMEOUT_MS);
    char *out = NULL;
    if (esp_websocket_client_send_text(d->ws, req, strlen(req), pdMS_TO_TICKS(TIMEOUT_MS)) >= 0 &&
        wait(&d->c, DONE_BIT, deadline)) { out = d->c.result; d->c.result = NULL; }
    free(req);
    return out;
}

void dash_ws_close(dash_ws_t *d)
{
    if (!d) return;
    if (d->ws) {
        esp_websocket_client_stop(d->ws);
        esp_websocket_client_destroy(d->ws);   /* no more events after this */
    }
    free(d->c.buf);
    free(d->c.result);
    if (d->c.ev) vEventGroupDelete(d->c.ev);
    free(d);
}

char *dash_fetch_config(void)
{
    dash_ws_t *d = dash_ws_open();
    char *out = dash_ws_call(d, "\"type\":\"lovelace/config\",\"url_path\":\"" HA_DASHBOARD "\"");
    if (!out) ESP_LOGW(TAG, "dashboard fetch failed");
    dash_ws_close(d);
    return out;
}
