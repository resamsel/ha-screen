/* Live entity states from HA's websocket subscribe_entities (compressed state messages).
 * Pure parsing, no I/O: builds on the host for tests. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct live live_t;

/* A cache for these entity ids (copied); NULL when out of memory. */
live_t *live_new(const char *const *ids, int n);
void live_free(live_t *l);
int live_count(const live_t *l);
const char *live_id(const live_t *l, int i);
/* Index of entity id, -1 when not tracked */
int live_find(const live_t *l, const char *id);

/* Applies one subscribe_entities event message ("a" = full states, "c" = changes with
 * "+"/"-", "r" = removed). Writes the indices of entities whose state changed to changed[]
 * (up to max, each once) and returns their number; 0 for anything else. */
int live_apply(live_t *l, const char *msg, int *changed, int max);

/* Entity i as a /api/states-like JSON object ("state", "attributes" with friendly_name,
 * unit_of_measurement, icon, temperature, humidity, temperature_unit, min, max), so the
 * REST parsers can read it. Missing values are left out. False if i is unknown. */
/* An input_text entity's whole state (unescaped, up to 255 chars); NULL for other entities or before the first state */
const char *live_text(const live_t *l, int i);
bool live_json(const live_t *l, int i, char *out, size_t n);
