#include "layout_store.h"
#include <string.h>
#include "nvs.h"
#include "esp_log.h"

#define NS  "layout"
#define KEY "v4"   /* bump when ha_layout_t changes */

bool layout_store_load(ha_layout_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, KEY, out, &n);
    nvs_close(h);
    if (err != ESP_OK || n != sizeof(*out)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return layout_sanitize(out);   /* never trust stored values blindly */
}

void layout_store_save(const ha_layout_t *l)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "v1");   /* older layout formats: free their space */
    nvs_erase_key(h, "v2");
    nvs_erase_key(h, "v3");
    esp_err_t err = nvs_set_blob(h, KEY, l, sizeof(*l));
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGW("layout", "could not save layout to NVS (%u bytes): %s", (unsigned)sizeof(*l), esp_err_to_name(err));
    nvs_close(h);
}
