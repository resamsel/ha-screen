/* IANA time zone names to POSIX TZ strings (newlib has no zone database) */
#pragma once

/* POSIX TZ for an IANA name like "Europe/London", or NULL if not in the table */
const char *tz_posix(const char *iana);
