/* OTA firmware updates (issue #14): download an image over HTTPS into the other app slot,
 * then reboot into it. The bootloader rolls back when the new image doesn't confirm itself. */
#pragma once
#include <stdbool.h>

typedef enum { OTA_CHECKING, OTA_DOWNLOADING, OTA_UP_TO_DATE, OTA_FAILED, OTA_REBOOTING, OTA_AVAILABLE } ota_state_t;

/* Progress: pct is 0..100 while downloading; msg a short status text. Called from the OTA task */
typedef void (*ota_cb_t)(ota_state_t state, int pct, const char *msg);

/* At boot: if this image is new and on trial, confirm it after HA_OTA_CONFIRM_MS of uptime */
void ota_boot(void);

/* Checks url in a background task (pausing the HA client). When it holds a newer version
 * (or the icons need repair) the board restarts into update mode; false when a check is
 * already running */
bool ota_start(const char *url, ota_cb_t cb);

/* Like ota_start, but only reads the version at url: reports OTA_AVAILABLE or OTA_UP_TO_DATE
 * and never restarts (issue #45) */
bool ota_check(const char *url, ota_cb_t cb);

/* The version the last check found at the url, "" before one succeeded */
const char *ota_latest(void);

/* At boot, after nvs_flash_init and the display, before the UI and HA: when a check asked
 * for update mode, downloads the update with nothing else running and restarts; otherwise
 * returns at once. cb shows the progress */
void ota_run_pending(ota_cb_t cb);

/* The last update mode's outcome ("Updated to v1.2.0", "Download failed"), once */
#include <stddef.h>
bool ota_last_result(char *out, size_t len);

/* The running firmware's version (the git tag it was built from) */
const char *ota_version(void);
