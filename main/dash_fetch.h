/* Reads a Home Assistant dashboard config (and other websocket API results). */
#pragma once

/* Returns the malloc'd lovelace/config result message for HA_DASHBOARD, or NULL. Caller frees. */
char *dash_fetch_config(void);

/* A short authenticated websocket session for several requests in a row */
typedef struct dash_ws dash_ws_t;
dash_ws_t *dash_ws_open(void);                         /* NULL on failure */
/* body: the request without its id, e.g. "\"type\":\"get_config\"". Returns the malloc'd
 * result message (caller frees) or NULL. */
char *dash_ws_call(dash_ws_t *d, const char *body);
void dash_ws_close(dash_ws_t *d);
