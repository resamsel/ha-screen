/* Last good dashboard layout, kept in NVS so the board starts with it when HA is unreachable. */
#pragma once
#include "layout.h"

bool layout_store_load(ha_layout_t *out);   /* false if missing or not valid */
void layout_store_save(const ha_layout_t *l);
