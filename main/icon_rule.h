/* Which Material Design icon an entity shows, the way Home Assistant picks it when the card
 * and the entity set none: HA's icon tables (websocket frontend/get_icons) give a default
 * icon plus icons for states ("off") or number ranges (battery levels). */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* From a frontend/get_icons result message, the entry resources.<integration>.<domain>.<key>
 * (category "entity"), or resources.<integration>.<key> with domain NULL (category
 * "entity_component", key = device class or "_"), as a compact rule string (see
 * icon_rule_apply). False when there is no such entry. */
bool icon_rule_from_icons(const char *msg, const char *integration, const char *domain, const char *key,
                          char *out, size_t n);

/* The icon a rule gives for an entity state: an exact state match, else the highest range
 * threshold <= the numeric state, else the default. "" when the rule is empty. */
void icon_rule_apply(const char *rule, const char *state, char *out, size_t n);

/* The JSON value of `key` in the object starting at obj ('{'); NULL when missing.
 * Only the object's own keys count, not nested ones. */
const char *json_obj_get(const char *obj, const char *key);

/* Copies the JSON string at p (pointing at '"') into out; false when p isn't a string */
bool json_get_str(const char *p, char *out, size_t n);
