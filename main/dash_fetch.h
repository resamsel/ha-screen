/* Reads a Home Assistant dashboard config over a short websocket connection. */
#pragma once

/* Returns the malloc'd lovelace/config result message for HA_DASHBOARD, or NULL. Caller frees. */
char *dash_fetch_config(void);
