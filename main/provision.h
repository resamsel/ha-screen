/* Wi-Fi and Home Assistant settings kept in NVS (issue #15). Values set at build time
 * (WIFI_SSID, WIFI_PASS, HA_TOKEN environment variables, HA_URL_DEFAULT) are only defaults
 * for keys the board has not stored yet. A setup portal (open Wi-Fi access point
 * "screen_xxxxxx" (the board ID) with a web form at http://192.168.4.1) stores new values. */
#pragma once
#include <stdbool.h>

void prov_load(void);            /* after nvs_flash_init, before anything below */
bool prov_ready(void);           /* SSID and token set */

const char *prov_ssid(void);
const char *prov_pass(void);
const char *prov_url(void);      /* no trailing slash */
const char *prov_token(void);
const char *prov_bearer(void);   /* "Bearer <token>" */

/* Ask for the portal on the next boot, with the reason shown at the top of the form (used
 * when Wi-Fi or HA never worked since boot) */
void prov_request_portal(const char *reason);
/* True once if a portal was requested; clears the request (the reason stays for the form) */
bool prov_portal_requested(void);

/* Run the setup portal. Never returns: restarts after the form is saved, or, when the
 * board already has settings, after `idle_timeout_s` without a saved form. `ap_name`
 * receives the access point's name (at least 24 bytes) before the portal starts, for the
 * screen. */
void prov_portal_run(int idle_timeout_s, void (*shown)(const char *ap_name));
