/* Host tests for main/cal_parse.c. Build/run: see test/run.sh */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cal_parse.h"

/* feeds s in chunks of k bytes */
static void feed(cal_parse_t *p, const char *s, int k)
{
    cal_parse_init(p);
    int n = strlen(s);
    for (int i = 0; i < n; i += k) cal_parse_feed(p, s + i, n - i < k ? n - i : k);
}

static void test_time(void)
{
    bool ad;
    assert(cal_parse_time("2026-11-23T10:00:00+01:00", &ad) == 1795424400 && !ad);   /* 09:00 UTC */
    assert(cal_parse_time("2026-11-23T09:00:00Z", &ad) == 1795424400 && !ad);
    assert(cal_parse_time("2026-11-23T04:00:00-05:00", &ad) == 1795424400);
    assert(cal_parse_time("2026-11-23T09:00:00.000+00:00", &ad) == 1795424400);
    assert(cal_parse_time("2026-11-23", &ad) == 1795392000 && ad);   /* TZ=UTC midnight */
    assert(cal_parse_time("garbage", &ad) == 0);
}

static const char RESP[] =
    "{\"changed_states\":[],\"service_response\":{"
    "\"calendar.family\":{\"events\":["
      "{\"start\":\"2026-11-24T18:00:00+01:00\",\"end\":\"2026-11-24T19:00:00+01:00\",\"summary\":\"Elternabend\","
       "\"description\":\"bring {cake} ] and \\\"stuff\\\", \\\\ \\\"events\\\":[\",\"location\":\"Schule\"},"
      "{\"start\":\"2026-11-23\",\"end\":\"2026-11-24\",\"summary\":\"M\\u00fcll \\u20ac\"}"
    "]},"
    "\"calendar.work\":{\"events\":["
      "{\"summary\":\"Standup\",\"start\":\"2026-11-23T08:30:00Z\",\"end\":\"2026-11-23T08:45:00Z\",\"extra\":{\"a\":[1,{\"b\":\"}\"}]}},"
      "{\"start\":\"2026-11-25T10:00:00+00:00\",\"end\":\"2026-11-25T11:00:00+00:00\",\"summary\":\"Gr\xc3\xbc\xc3\x9f" "e\"}"
    "]}}}";

static void test_events(void)
{
    for (int k = 1; k <= 64; k *= 2) {   /* any chunking gives the same result */
        cal_parse_t p = { .ids = "calendar.work,calendar.family" };
        feed(&p, RESP, k);
        assert(p.ev[0].src == 1 && p.ev[1].src == 0 && p.ev[2].src == 1 && p.ev[3].src == 0);   /* its calendar */
        assert(p.n == 4);
        assert(!strcmp(p.ev[0].summary, "M\xc3\xbcll \xe2\x82\xac") && p.ev[0].all_day);   /* all-day: midnight */
        assert(!strcmp(p.ev[1].summary, "Standup") && p.ev[1].start == 1795422600 && p.ev[1].end == 1795423500);
        assert(!strcmp(p.ev[2].summary, "Elternabend") && !p.ev[2].all_day);
        assert(!strcmp(p.ev[3].summary, "Gr\xc3\xbc\xc3\x9f" "e"));
    }
}

static void test_limits(void)
{
    /* more events than kept: the earliest win; long summaries are cut on a UTF-8 boundary */
    char buf[8192];
    int n = snprintf(buf, sizeof(buf), "{\"service_response\":{\"calendar.x\":{\"events\":[");
    for (int i = 12; i >= 1; i--)
        n += snprintf(buf + n, sizeof(buf) - n, "%s{\"start\":\"2026-12-%02dT10:00:00Z\",\"summary\":\"E%d\"}", i == 12 ? "" : ",", i, i);
    n += snprintf(buf + n, sizeof(buf) - n, ",{\"start\":\"2026-12-20T10:00:00Z\",\"summary\":\"");
    for (int i = 0; i < 40; i++) n += snprintf(buf + n, sizeof(buf) - n, "\xc3\xa4");   /* 80 bytes of ä */
    snprintf(buf + n, sizeof(buf) - n, "\"},{\"summary\":\"no start\"}]}}}");
    cal_parse_t p;
    feed(&p, buf, 7);
    assert(p.n == CAL_MAX_EVENTS);
    for (int i = 0; i < CAL_MAX_EVENTS; i++) {
        char want[8];
        snprintf(want, sizeof(want), "E%d", i + 1);
        assert(!strcmp(p.ev[i].summary, want));
    }
    buf[0] = 0;
    snprintf(buf, sizeof(buf), "{\"x\":{\"events\":[{\"start\":\"2026-12-20T10:00:00Z\",\"summary\":\"");
    for (int i = 0; i < 40; i++) strcat(buf, "\xc3\xa4");
    strcat(buf, "\"},{\"start\":\"2026-12-21T10:00:00Z\"}]}}");
    feed(&p, buf, 5);
    assert(p.n == 2);
    size_t l = strlen(p.ev[0].summary);
    assert(l < CAL_SUMMARY_LEN && l % 2 == 0 && l >= 40);   /* whole characters only */
    assert(!strcmp(p.ev[1].summary, "(no title)"));
    feed(&p, "{\"service_response\":{\"calendar.x\":{\"events\":[]}}}", 3);
    assert(p.n == 0);
}

static void test_mask(void)
{
    /* month view: days with events of November 2026 (TZ=UTC), every event counts, multi-day too */
    cal_parse_t p = { .mask_from = 1793491200 };   /* 2026-11-01 */
    feed(&p, RESP, 9);
    assert(p.mask_from == 1793491200);
    assert(p.mask == (1u << 22 | 1u << 23 | 1u << 24));   /* 23rd, 24th, 25th */
    feed(&p, "{\"x\":{\"events\":[{\"start\":\"2026-11-29\",\"end\":\"2026-12-03\"},"
             "{\"start\":\"2026-12-05T10:00:00Z\",\"end\":\"2026-12-05T11:00:00Z\"}]}}", 11);
    assert(p.mask == (1u << 28 | 1u << 29));   /* 29th, 30th; December not */
}

int main(void)
{
    test_mask();
    test_time();
    test_events();
    test_limits();
    printf("cal_parse tests passed\n");
    return 0;
}
