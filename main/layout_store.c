#include "layout_store.h"
#include <string.h>
#include <stdlib.h>
#include "nvs.h"
#include "cache_store.h"

/* The layout lives in the flash cache partition: at ~10 KB it no longer fits in NVS */
#define VERSION 7   /* bump when ha_layout_t changes */

bool layout_store_load(ha_layout_t *out)
{
    size_t n;
    uint32_t tag;
    void *b = cache_read(CACHE_LAYOUT, &n, &tag);
    bool ok = b && tag == VERSION && n == sizeof(*out);
    if (ok) memcpy(out, b, sizeof(*out));
    else memset(out, 0, sizeof(*out));
    free(b);
    return ok && layout_sanitize(out);   /* never trust stored values blindly */
}

void layout_store_save(const ha_layout_t *l)
{
    static bool cleaned;
    if (!cleaned) {                       /* layouts kept in NVS by older firmware: free it */
        nvs_handle_t h;
        if (nvs_open("layout", NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
        cleaned = true;
    }
    cache_write(CACHE_LAYOUT, l, sizeof(*l), VERSION);
}
