#pragma once
#include <stdbool.h>
#include <stddef.h>

/* An input_text holds up to 255 bytes */
#define NOTIFY_MAX 256

/* The notification helper's state → the text to show. False for "", unknown, unavailable.
 * Cut to fit n on a UTF-8 character boundary. */
bool notify_text(const char *state, char *out, size_t n);

/* The last message handed to the screen: a resync re-sends the helper's state, which must not
 * show (and restart the timer of) the same message again. An empty helper forgets it. */
typedef struct { char last[NOTIFY_MAX]; } notify_seen_t;

/* notify_text, but true only for a message other than the last one shown */
bool notify_new(notify_seen_t *seen, const char *state, char *out, size_t n);
