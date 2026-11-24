#include "ota.h"
#include "config.h"
#include "icons.h"
#include "ha_client.h"
#include "provision.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_http_client.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";
static volatile bool s_busy;
static char s_url[256];
static ota_cb_t s_cb;
static bool s_check_only;        /* ota_check: report, never restart */
static char s_latest[32];
const char *ota_latest(void) { return s_latest; }

const char *ota_version(void) { return esp_app_get_description()->version; }

static void confirm(void *arg)
{
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) ESP_LOGI(TAG, "image confirmed");
}

void ota_boot(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "running %s from %s", ota_version(), run ? run->label : "?");
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) return;
    /* On trial: survived HA_OTA_CONFIRM_MS without a crash or watchdog reset -> keep it */
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = { .callback = confirm, .name = "ota_ok" };
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, (uint64_t)HA_OTA_CONFIRM_MS * 1000);
}

static void report(ota_state_t s, int pct, const char *msg)
{
    ESP_LOGI(TAG, "%s", msg);
    if (s_cb) s_cb(s, pct, msg);
}

/* ---- Icons: the blob next to the image (same release, "mdi_icons.bin") ----
 * The icons partition isn't part of the app image, so an update streams the release's blob
 * and compares it with the partition; only when they differ is it downloaded again and
 * written. The 4-byte magic goes last, so an interrupted write leaves "no icons" (icons.c
 * checks it), not garbage; the next update writes it again. */
#define ICONS_MAGIC_LEN 4

static esp_http_client_handle_t icons_open(const char *url, int *len)
{
    esp_http_client_config_t c = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000,
        .buffer_size = 4096, .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return NULL;
    for (int hop = 0; hop < 5; hop++) {          /* GitHub redirects to its download host */
        if (esp_http_client_open(h, 0) != ESP_OK) break;
        *len = esp_http_client_fetch_headers(h);
        int st = esp_http_client_get_status_code(h);
        if (st == 200 && *len > 0) return h;
        if (st != 301 && st != 302 && st != 303 && st != 307 && st != 308) break;
        esp_http_client_set_redirection(h);
        esp_http_client_close(h);
    }
    esp_http_client_cleanup(h);
    return NULL;
}

static bool icons_valid(const esp_partition_t *p)
{
    char m[ICONS_MAGIC_LEN];
    return esp_partition_read(p, 0, m, sizeof m) == ESP_OK && !memcmp(m, "MDI1", 4);
}

/* write=false: 1 when the download differs from the partition, 0 same, -1 error.
 * write=true: erases and writes it; 0 ok, -1 error. */
static int icons_pass(const esp_partition_t *p, const char *url, bool write)
{
    int len = 0, res = -1, off = 0, last = -1;
    esp_http_client_handle_t h = icons_open(url, &len);
    if (!h) return -1;
    uint8_t *buf = malloc(4096), *cur = malloc(4096), magic[ICONS_MAGIC_LEN];
    if (!buf || !cur || len > (int)p->size) goto out;
    if (!write && !icons_valid(p)) { res = 1; goto out; }
    if (write) {          /* the UI stops reading the blob before it is erased */
        icons_disable();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (write && esp_partition_erase_range(p, 0, (len + 4095) & ~4095) != ESP_OK) goto out;
    while (off < len) {
        int n = esp_http_client_read(h, (char *)buf, 4096);
        if (n <= 0) goto out;
        if (write) {
            int skip = 0;
            if (off < ICONS_MAGIC_LEN) {       /* hold the magic back until the end */
                skip = ICONS_MAGIC_LEN - off < n ? ICONS_MAGIC_LEN - off : n;
                memcpy(magic + off, buf, skip);
            }
            if (n > skip && esp_partition_write(p, off + skip, buf + skip, n - skip) != ESP_OK) goto out;
            int pct = (int)((int64_t)(off + n) * 100 / len);
            if (pct != last) {
                char msg[32];
                last = pct;
                snprintf(msg, sizeof msg, "Icons %d%%", pct);
                report(OTA_DOWNLOADING, pct, msg);
            }
        } else {
            if (esp_partition_read(p, off, cur, n) != ESP_OK) goto out;
            if (memcmp(cur, buf, n)) { res = 1; goto out; }
        }
        off += n;
    }
    if (write) res = esp_partition_write(p, 0, magic, ICONS_MAGIC_LEN) == ESP_OK ? 0 : -1;
    else {
        /* same bytes; a longer old blob only matters if its tail is read, which the
         * header's counts prevent */
        res = 0;
    }
out:
    free(buf); free(cur);
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return res;
}

/* Brings the icons partition up to the release's blob. only_if_broken: skip unless the
 * partition has no valid blob. Returns true when it wrote (a restart then shows them) */
static bool icons_update(bool only_if_broken, bool *failed)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "icons");
    *failed = false;
    if (!p || (only_if_broken && icons_valid(p))) return false;
    char url[sizeof s_url];
    snprintf(url, sizeof url, "%s", s_url);
    char *slash = strrchr(url, '/');
    if (!slash || slash + 1 - url + sizeof "mdi_icons.bin" > sizeof url) { *failed = true; return false; }
    strcpy(slash + 1, "mdi_icons.bin");
    report(OTA_CHECKING, 0, "Checking icons...");
    int diff = icons_pass(p, url, false);
    if (diff <= 0) { *failed = diff < 0; return false; }
    if (icons_pass(p, url, true) < 0) { *failed = true; return false; }
    ESP_LOGI(TAG, "icons updated");
    return true;
}

static esp_err_t ota_begin(esp_http_client_config_t *http, esp_https_ota_handle_t *h)
{
    *http = (esp_http_client_config_t){
        .url = s_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .buffer_size = 4096,       /* GitHub's redirect carries long headers */
        .buffer_size_tx = 2048,
    };
    esp_https_ota_config_t cfg = { .http_config = http };
    return esp_https_ota_begin(&cfg, h);
}

static void heap_log(const char *when)
{
    ESP_LOGI(TAG, "%s: heap free %u, largest block %u", when, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

/* ---- Update mode ----
 * A download streams ~4.6 MB through TLS, whose 16 KB record buffer is freed and allocated
 * again for every record (CONFIG_MBEDTLS_DYNAMIC_BUFFER). On the running dashboard other
 * allocations (Wi-Fi buffers, the UI) soon split that block and the download dies. So the
 * check runs in normal mode, and the download after a restart into "update mode": no UI
 * pages, no HA client, just Wi-Fi - a clean heap. */
#define NVS_NS "ota"

static void nvs_put(const char *key, const char *val)
{
    nvs_handle_t nv;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nv) != ESP_OK) return;
    if (val) nvs_set_str(nv, key, val); else nvs_erase_key(nv, key);
    nvs_commit(nv);
    nvs_close(nv);
}

static bool nvs_take(const char *key, char *out, size_t len)   /* read and erase */
{
    nvs_handle_t nv;
    bool ok = false;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nv) != ESP_OK) return false;
    if (nvs_get_str(nv, key, out, &len) == ESP_OK) {
        ok = true;
        nvs_erase_key(nv, key);
        nvs_commit(nv);
    }
    nvs_close(nv);
    return ok;
}

static void restart_into_update(const char *why)
{
    nvs_put("url", s_url);
    report(OTA_REBOOTING, 0, why);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

/* Normal mode: is there anything to do? Then restart into update mode */
static void check_task(void *arg)
{
    esp_http_client_config_t http;
    esp_https_ota_handle_t h = NULL;
    esp_app_desc_t desc;
    char msg[64];
    report(OTA_CHECKING, 0, "Checking...");
    /* HA's TLS session and GitHub's don't both fit in RAM (16 KB record buffer each) */
    if (!ha_client_pause(30000)) ESP_LOGW(TAG, "HA client still busy; trying anyway");
    heap_log("check");
    esp_err_t err = ota_begin(&http, &h);
    if (err != ESP_OK) snprintf(msg, sizeof msg, "Download failed");
    else if ((err = esp_https_ota_get_img_desc(h, &desc)) != ESP_OK) snprintf(msg, sizeof msg, "Bad image");
    if (h) esp_https_ota_abort(h);
    if (err == ESP_OK) {
        const esp_partition_t *ip = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "icons");
        snprintf(s_latest, sizeof s_latest, "%s", desc.version);
        bool newer = strcmp(desc.version, ota_version()) != 0;
        if (s_check_only) {
            if (newer) snprintf(msg, sizeof msg, "%.20s available", desc.version);
            else snprintf(msg, sizeof msg, "Up to date");
            report(newer ? OTA_AVAILABLE : OTA_UP_TO_DATE, 100, msg);
            ha_client_resume();
            s_busy = false;
            vTaskDelete(NULL);
        }
        if (newer) {
            snprintf(msg, sizeof msg, "Updating to %.20s", desc.version);
            restart_into_update(msg);
        }
        if (ip && !icons_valid(ip)) restart_into_update("Repairing icons");
        snprintf(msg, sizeof msg, "Up to date (%.20s)", desc.version);
    } else ESP_LOGW(TAG, "check failed: %s", esp_err_to_name(err));
    report(err == ESP_OK ? OTA_UP_TO_DATE : OTA_FAILED, 100, msg);
    ha_client_resume();
    s_busy = false;
    vTaskDelete(NULL);
}

static bool start(const char *url, ota_cb_t cb, bool check_only)
{
    if (s_busy) return false;
    s_busy = true;
    s_check_only = check_only;
    snprintf(s_url, sizeof s_url, "%s", url);
    s_cb = cb;
    if (xTaskCreate(check_task, "ota", 8192, NULL, 4, NULL) != pdPASS) { s_busy = false; return false; }
    return true;
}

bool ota_start(const char *url, ota_cb_t cb) { return start(url, cb, false); }
bool ota_check(const char *url, ota_cb_t cb) { return start(url, cb, true); }

static EventGroupHandle_t s_wifi_ev;

static void wifi_ev(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) esp_wifi_connect();
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(s_wifi_ev, BIT0);
}

static bool wifi_up(void)
{
    s_wifi_ev = xEventGroupCreate();
    if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK) return false;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) return false;
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_ev, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_ev, NULL);
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, prov_ssid(), sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, prov_pass(), sizeof(wc.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (esp_wifi_start() != ESP_OK) return false;
    return xEventGroupWaitBits(s_wifi_ev, BIT0, pdFALSE, pdTRUE, pdMS_TO_TICKS(60000)) & BIT0;
}

/* Update mode: download app and icons, store the outcome for the Firmware page, restart */
static void update(void)
{
    esp_http_client_config_t http;
    esp_https_ota_handle_t h = NULL;
    esp_app_desc_t desc = {0};
    char msg[64];
    bool icons_failed;
    esp_err_t err = ESP_FAIL;
    report(OTA_CHECKING, 0, "Connecting...");
    if (!wifi_up()) { snprintf(msg, sizeof msg, "Update failed: no Wi-Fi"); goto out; }
    heap_log("update");
    if ((err = ota_begin(&http, &h)) != ESP_OK) { snprintf(msg, sizeof msg, "Download failed"); goto out; }
    if ((err = esp_https_ota_get_img_desc(h, &desc)) != ESP_OK) { snprintf(msg, sizeof msg, "Bad image"); goto abort; }
    if (!strcmp(desc.version, ota_version())) {         /* only the icons (repair) */
        esp_https_ota_abort(h);
        icons_update(false, &icons_failed);
        snprintf(msg, sizeof msg, icons_failed ? "Icons failed" : "Icons updated");
        goto out;
    }
    int total = esp_https_ota_get_image_size(h), last = -1;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int got = esp_https_ota_get_image_len_read(h);
        int pct = total > 0 ? (int)((int64_t)got * 100 / total) : 0;
        if (pct != last) {
            last = pct;
            snprintf(msg, sizeof msg, "%.20s %d%%", desc.version, pct);
            report(OTA_DOWNLOADING, pct, msg);
        }
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        heap_log("failed");
        snprintf(msg, sizeof msg, "Download failed");
        goto abort;
    }
    if ((err = esp_https_ota_finish(h)) != ESP_OK) { snprintf(msg, sizeof msg, "Image invalid"); goto out; }
    /* The new app boots on the restart; bring its icons along. A failure here still
     * restarts into the new app (no icons until the next Update repairs them) */
    icons_update(false, &icons_failed);
    snprintf(msg, sizeof msg, icons_failed ? "Updated to %.20s, icons failed" : "Updated to %.20s", desc.version);
    goto out;
abort:
    esp_https_ota_abort(h);
out:
    if (err != ESP_OK) ESP_LOGW(TAG, "update failed: %s", esp_err_to_name(err));
    report(err == ESP_OK ? OTA_REBOOTING : OTA_FAILED, 100, msg);
    nvs_put("msg", msg);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

void ota_run_pending(ota_cb_t cb)
{
    if (!nvs_take("url", s_url, sizeof s_url)) return;   /* erased first: a crash can't loop */
    s_cb = cb;
    prov_load();
    ESP_LOGI(TAG, "update mode: %s", s_url);
    update();                                            /* restarts */
}

bool ota_last_result(char *out, size_t len) { return nvs_take("msg", out, len); }
