#include "ota.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";
static volatile bool s_busy;
static char s_url[256];
static ota_cb_t s_cb;

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

static void ota_task(void *arg)
{
    esp_http_client_config_t http = {
        .url = s_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .buffer_size = 4096,       /* GitHub's redirect carries long headers */
        .buffer_size_tx = 2048,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_https_ota_handle_t h = NULL;
    char msg[64];
    report(OTA_CHECKING, 0, "Checking...");
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK) { snprintf(msg, sizeof msg, "Download failed"); goto fail; }

    esp_app_desc_t desc;
    if (esp_https_ota_get_img_desc(h, &desc) != ESP_OK) { snprintf(msg, sizeof msg, "Bad image"); goto abort; }
    if (!strcmp(desc.version, ota_version())) {
        esp_https_ota_abort(h);
        snprintf(msg, sizeof msg, "Up to date (%.20s)", desc.version);
        report(OTA_UP_TO_DATE, 100, msg);
        goto done;
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
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) { snprintf(msg, sizeof msg, "Download failed"); goto abort; }
    if (esp_https_ota_finish(h) != ESP_OK) { snprintf(msg, sizeof msg, "Image invalid"); h = NULL; goto fail; }
    snprintf(msg, sizeof msg, "Restarting (%.20s)", desc.version);
    report(OTA_REBOOTING, 100, msg);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

abort:
    esp_https_ota_abort(h);
fail:
    ESP_LOGW(TAG, "update failed: %s", esp_err_to_name(err));
    report(OTA_FAILED, 0, msg);
done:
    s_busy = false;
    vTaskDelete(NULL);
}

bool ota_start(const char *url, ota_cb_t cb)
{
    if (s_busy) return false;
    s_busy = true;
    snprintf(s_url, sizeof s_url, "%s", url);
    s_cb = cb;
    if (xTaskCreate(ota_task, "ota", 8192, NULL, 4, NULL) != pdPASS) { s_busy = false; return false; }
    return true;
}
