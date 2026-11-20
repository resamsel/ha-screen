#include "cache_store.h"
#include <string.h>
#include <stdlib.h>
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_log.h"

/* The "cache" partition (partitions.csv): per area two slots, written alternately so each
 * sector sees half the writes; a read takes the newest slot whose CRC checks out. */
#define MAGIC 0x48414331u   /* "HAC1" */
static const char *TAG = "cache";

typedef struct {
    uint32_t magic, seq, len, crc, tag;
} hdr_t;

static const struct { uint32_t off, size; } AREA[CACHE_AREAS] = {
    [CACHE_LAYOUT] = { 0x00000, 0x08000 },   /* 2 x 16 KB */
    [CACHE_DATA]   = { 0x10000, 0x20000 },   /* 2 x 64 KB */
};

static const esp_partition_t *part(void)
{
    static const esp_partition_t *p;
    if (!p) p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "cache");
    return p;
}

static bool slot_hdr(int area, int s, hdr_t *h)
{
    const esp_partition_t *p = part();
    uint32_t slot = AREA[area].size / 2;
    if (!p || esp_partition_read(p, AREA[area].off + s * slot, h, sizeof(*h)) != ESP_OK) return false;
    return h->magic == MAGIC && h->len <= slot - sizeof(*h);
}

void *cache_read(int area, size_t *len, uint32_t *tag)
{
    const esp_partition_t *p = part();
    if (!p || area < 0 || area >= CACHE_AREAS) return NULL;
    uint32_t slot = AREA[area].size / 2;
    hdr_t h[2];
    bool ok[2] = { slot_hdr(area, 0, &h[0]), slot_hdr(area, 1, &h[1]) };
    for (int k = 0; k < 2; k++) {        /* newest first, the other if it is damaged */
        int s = ok[0] && ok[1] ? ((int32_t)(h[1].seq - h[0].seq) > 0) ^ k : ok[k] ? k : -1;
        if (s < 0 || !ok[s]) continue;
        void *buf = malloc(h[s].len ? h[s].len : 1);
        if (!buf) return NULL;
        if (esp_partition_read(p, AREA[area].off + s * slot + sizeof(hdr_t), buf, h[s].len) == ESP_OK &&
            esp_rom_crc32_le(0, buf, h[s].len) == h[s].crc) {
            *len = h[s].len;
            if (tag) *tag = h[s].tag;
            return buf;
        }
        free(buf);
    }
    return NULL;
}

bool cache_write(int area, const void *data, size_t len, uint32_t tag)
{
    const esp_partition_t *p = part();
    if (!p || area < 0 || area >= CACHE_AREAS) return false;
    uint32_t slot = AREA[area].size / 2;
    if (len > slot - sizeof(hdr_t)) { ESP_LOGW(TAG, "area %d: %u bytes too large", area, (unsigned)len); return false; }
    hdr_t h[2], n = { MAGIC, 1, len, esp_rom_crc32_le(0, data, len), tag };
    bool ok[2] = { slot_hdr(area, 0, &h[0]), slot_hdr(area, 1, &h[1]) };
    int newest = ok[0] && ok[1] ? (int32_t)(h[1].seq - h[0].seq) > 0 : ok[1] ? 1 : ok[0] ? 0 : -1;
    if (newest >= 0) {
        if (h[newest].crc == n.crc && h[newest].len == len && h[newest].tag == tag) return true;   /* unchanged */
        n.seq = h[newest].seq + 1;
    }
    int s = newest == 0 ? 1 : 0;
    uint32_t at = AREA[area].off + s * slot, sz = (sizeof(hdr_t) + len + 4095) & ~4095u;
    /* header last: a slot interrupted while writing never looks valid */
    if (esp_partition_erase_range(p, at, sz) != ESP_OK ||
        esp_partition_write(p, at + sizeof(hdr_t), data, len) != ESP_OK ||
        esp_partition_write(p, at, &n, sizeof(n)) != ESP_OK) {
        ESP_LOGW(TAG, "area %d: write failed", area);
        return false;
    }
    ESP_LOGI(TAG, "area %d: saved %u bytes (slot %d)", area, (unsigned)len, s);
    return true;
}

bool cache_available(void) { return part() != NULL; }
