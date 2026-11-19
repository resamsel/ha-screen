#include "layout_store.h"
#include <string.h>
#include "nvs.h"
#include "esp_log.h"

#define NS  "layout"
#define KEY "v1"   /* bump when ha_layout_t changes */

bool layout_store_load(ha_layout_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, KEY, out, &n);
    nvs_close(h);
    if (err != ESP_OK || n != sizeof(*out) || out->count < 1 || out->count > LAYOUT_MAX_PAGES) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    for (int p = 0; p < out->count; p++) {   /* never trust stored sizes blindly */
        layout_page_t *pg = &out->pages[p];
        if (pg->count < 1 || pg->count > LAYOUT_MAX_ROWS) { memset(out, 0, sizeof(*out)); return false; }
        pg->title[sizeof(pg->title) - 1] = 0;
        for (int i = 0; i < pg->count; i++) {
            pg->items[i].entity[sizeof(pg->items[i].entity) - 1] = 0;
            pg->items[i].name[sizeof(pg->items[i].name) - 1] = 0;
        }
    }
    return true;
}

void layout_store_save(const ha_layout_t *l)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, KEY, l, sizeof(*l)) != ESP_OK || nvs_commit(h) != ESP_OK)
        ESP_LOGW("layout", "could not save layout to NVS");
    nvs_close(h);
}
