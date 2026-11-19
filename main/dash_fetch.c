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
    char *result;   /* complete id=1 message */
} ctx_t;

static void on_message(ctx_t *c, char *msg)
{
    if (strstr(msg, "\"type\":\"auth_required\"")) xEventGroupSetBits(c->ev, AUTH_REQ_BIT);
    else if (strstr(msg, "\"type\":\"auth_ok\"")) xEventGroupSetBits(c->ev, AUTH_OK_BIT);
    else if (strstr(msg, "\"type\":\"auth_invalid\"")) { ESP_LOGW(TAG, "auth rejected"); xEventGroupSetBits(c->ev, FAIL_BIT); }
    else if (strstr(msg, "\"id\":1") && !c->result) {
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

char *dash_fetch_config(void)
{
    char uri[160];
    const char *host = strstr(HA_URL, "://");
    snprintf(uri, sizeof(uri), "%s%s/api/websocket", strncmp(HA_URL, "https", 5) ? "ws" : "wss", host ? host : "");
    ctx_t c = { .ev = xEventGroupCreate() };
    if (!c.ev) return NULL;
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .network_timeout_ms = 5000,
        .disable_auto_reconnect = true,
    };
    esp_websocket_client_handle_t ws = esp_websocket_client_init(&cfg);
    if (!ws) { vEventGroupDelete(c.ev); return NULL; }
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event, &c);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TIMEOUT_MS);
    char *out = NULL;
    if (esp_websocket_client_start(ws) == ESP_OK && wait(&c, AUTH_REQ_BIT, deadline)) {
        const char *auth = "{\"type\":\"auth\",\"access_token\":\"" HA_TOKEN "\"}";
        esp_websocket_client_send_text(ws, auth, strlen(auth), pdMS_TO_TICKS(TIMEOUT_MS));
        if (wait(&c, AUTH_OK_BIT, deadline)) {
            const char *req = "{\"id\":1,\"type\":\"lovelace/config\",\"url_path\":\"" HA_DASHBOARD "\"}";
            esp_websocket_client_send_text(ws, req, strlen(req), pdMS_TO_TICKS(TIMEOUT_MS));
            if (wait(&c, DONE_BIT, deadline)) { out = c.result; c.result = NULL; }
        }
    }
    if (!out) ESP_LOGW(TAG, "dashboard fetch failed");
    esp_websocket_client_stop(ws);
    esp_websocket_client_destroy(ws);   /* no more events after this */
    free(c.buf);
    free(c.result);
    vEventGroupDelete(c.ev);
    return out;
}
