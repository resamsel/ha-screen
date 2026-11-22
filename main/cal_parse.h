/* Calendar card events from calendar.get_events (return_response), parsed while the
 * response streams in. Pure parsing, no I/O: builds on the host for tests. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CAL_MAX_EVENTS 8
#define CAL_SUMMARY_LEN 48

typedef struct {
    int64_t start, end;           /* UTC seconds; all-day: local midnight, end exclusive */
    bool all_day;
    uint8_t src;                  /* index of its calendar in cal_parse_t.ids (0 if not found) */
    uint32_t color;               /* 0xRRGGBB, filled in by the caller */
    char summary[CAL_SUMMARY_LEN];
} cal_event_t;

typedef struct {
    int n;                        /* events kept, earliest first */
    const char *ids;              /* set after init: the card's calendar ids, comma-separated */
    int64_t mask_from;            /* set after init: local midnight of the 1st of a month */
    uint32_t mask;                /* bit d-1: some event (any, not only those kept) is on day d of it */
    cal_event_t ev[CAL_MAX_EVENTS];
    /* streaming state */
    int mode, match, depth;       /* mode 0 = before "events":[, 1 = in the list, 2 = in an event */
    bool in_str, esc, want_key;
    int uni;                      /* hex digits still to read after \u, 0 = none */
    uint32_t cp;
    char key[16], val[CAL_SUMMARY_LEN + 8];
    int klen, vlen;
    bool vtrunc;
    cal_event_t cur;
    uint8_t src;                  /* calendar of the list being read */
    bool s0;                      /* before a list: in a string */
    char last[64], prev[64];      /* before a list: the last two strings (the calendar id, "events") */
    int llen;
} cal_parse_t;

void cal_parse_init(cal_parse_t *p);   /* keeps mask_from and ids */
/* Feeds a chunk of the response; events of every calendar in it are merged by start time,
 * the earliest CAL_MAX_EVENTS kept. */
void cal_parse_feed(cal_parse_t *p, const char *data, int n);

/* "2026-11-23T10:00:00+01:00" (UTC from the offset; without one: local time) or
 * "2026-11-23" (local midnight, *all_day set). 0 if unreadable. */
int64_t cal_parse_time(const char *s, bool *all_day);
