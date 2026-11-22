#include "icons.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_partition.h"
#include "esp_log.h"
#endif

/* The blob, memory-mapped from the "icons" partition (issue #14: not in the app, so the app
 * fits an OTA slot). NULL = no icons (partition missing or not written) */
static const uint8_t *volatile mdi_start;

void icons_init(void)
{
#ifdef ESP_PLATFORM
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "icons");
    const void *ptr; esp_partition_mmap_handle_t h;
    if (!p || esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, &ptr, &h) != ESP_OK) {
        ESP_LOGW("icons", "no icons partition");
        return;
    }
    if (memcmp(ptr, "MDI1", 4)) { ESP_LOGW("icons", "icons partition is empty (idf.py flash writes it)"); return; }
    mdi_start = ptr;
#endif
}

void icons_disable(void) { mdi_start = NULL; }

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

const uint8_t *icon_bitmap(const char *name)
{
    const uint8_t *m = mdi_start;   /* one read: icons_disable() may clear it meanwhile */
    if (!m || !name || !name[0] || memcmp(m, "MDI1", 4) || rd32(m + 8) != ICON_SIZE) return NULL;
    if (!strncmp(name, "mdi:", 4)) name += 4;
    else if (strchr(name, ':')) return NULL;          /* other icon sets (hass:, custom) */
    uint32_t n = rd32(m + 4), sz = ICON_SIZE * ICON_SIZE / 2;
    const uint8_t *offs = m + 12, *bitmaps = offs + 4 * n, *names = bitmaps + n * sz;
    uint32_t lo = 0, hi = n;
    while (lo < hi) {                                  /* names are sorted */
        uint32_t mid = (lo + hi) / 2;
        int c = strcmp(name, (const char *)names + rd32(offs + 4 * mid));
        if (!c) return bitmaps + mid * sz;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

static int px4(const uint8_t *b, int x, int y)
{
    if (x < 0 || y < 0 || x >= ICON_SIZE || y >= ICON_SIZE) return 0;
    uint8_t v = b[(y * ICON_SIZE + x) / 2];
    return x & 1 ? v & 15 : v >> 4;
}

void icon_scale(const uint8_t *src, uint8_t *dst, int n)
{
    memset(dst, 0, n * n / 2);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            /* bilinear at the target pixel's centre (box average when shrinking) */
            int acc = 0, cnt = 0, s = n < ICON_SIZE ? (ICON_SIZE + n - 1) / n : 1;
            for (int sy = 0; sy < s; sy++)
                for (int sx = 0; sx < s; sx++) {
                    int fx = ((2 * x + 1) * ICON_SIZE * 256 / (2 * n)) - 128 + (sx * 256) / s - (s - 1) * 128 / s;
                    int fy = ((2 * y + 1) * ICON_SIZE * 256 / (2 * n)) - 128 + (sy * 256) / s - (s - 1) * 128 / s;
                    int ix = fx >> 8, iy = fy >> 8, ax = fx & 255, ay = fy & 255;
                    int top = px4(src, ix, iy) * (256 - ax) + px4(src, ix + 1, iy) * ax;
                    int bot = px4(src, ix, iy + 1) * (256 - ax) + px4(src, ix + 1, iy + 1) * ax;
                    acc += (top * (256 - ay) + bot * ay) >> 8;
                    cnt++;
                }
            int v = (acc / cnt + 128) >> 8;
            if (v > 15) v = 15;
            dst[(y * n + x) / 2] |= x & 1 ? v : v << 4;
        }
}
