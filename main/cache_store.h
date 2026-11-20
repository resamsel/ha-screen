/* Flash cache for the last dashboard layout and card data (the "cache" partition), so the
 * board shows them right after booting, before Wi-Fi and HA answer. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { CACHE_LAYOUT, CACHE_DATA, CACHE_AREAS };

/* The newest valid blob of an area (malloc'd, caller frees) and the tag saved with it;
 * NULL when there is none */
void *cache_read(int area, size_t *len, uint32_t *tag);

/* Saves a blob (skipped when it equals the newest one). False on failure. */
bool cache_write(int area, const void *data, size_t len, uint32_t tag);

bool cache_available(void);   /* false: the partition table has no "cache" partition */
