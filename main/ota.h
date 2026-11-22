/* OTA firmware updates (issue #14): download an image over HTTPS into the other app slot,
 * then reboot into it. The bootloader rolls back when the new image doesn't confirm itself. */
#pragma once
#include <stdbool.h>

typedef enum { OTA_CHECKING, OTA_DOWNLOADING, OTA_UP_TO_DATE, OTA_FAILED, OTA_REBOOTING } ota_state_t;

/* Progress: pct is 0..100 while downloading; msg a short status text. Called from the OTA task */
typedef void (*ota_cb_t)(ota_state_t state, int pct, const char *msg);

/* At boot: if this image is new and on trial, confirm it after HA_OTA_CONFIRM_MS of uptime */
void ota_boot(void);

/* Starts an update from url in a background task; false when one is already running */
bool ota_start(const char *url, ota_cb_t cb);

/* The running firmware's version (the git tag it was built from) */
const char *ota_version(void);
