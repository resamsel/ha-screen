#pragma once
#include <stdbool.h>
#include <stddef.h>

/* An input_text holds up to 255 bytes */
#define NOTIFY_MAX 256

/* The notification helper's state → the text to show. False for "", unknown, unavailable.
 * Cut to fit n on a UTF-8 character boundary. */
bool notify_text(const char *state, char *out, size_t n);
