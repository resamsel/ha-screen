#include "cal_parse.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char KEY[] = "\"events\":[";

void cal_parse_init(cal_parse_t *p)
{
    int64_t from = p->mask_from;
    const char *ids = p->ids;
    memset(p, 0, sizeof(*p));
    p->mask_from = from;
    p->ids = ids;
}

/* index of id in the comma-separated list, 0 if not in it */
static uint8_t id_index(const char *ids, const char *id)
{
    int k = 0;
    size_t n = strlen(id);
    for (const char *q = ids; q && *q; k++) {
        size_t m = strcspn(q, ",");
        if (m == n && !strncmp(q, id, n)) return k;
        q += m + (q[m] == ',');
    }
    return 0;
}

/* marks the days of the month at mask_from that [start, end) touches */
static void mark_days(cal_parse_t *p, const cal_event_t *e)
{
    if (!p->mask_from) return;
    int64_t end = e->end > e->start ? e->end : e->start + 1;
    for (int d = 0; d < 31; d++) {
        struct tm tm;
        time_t t = (time_t)p->mask_from;
        localtime_r(&t, &tm);
        int mon = tm.tm_mon;
        tm.tm_mday += d; tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0; tm.tm_isdst = -1;
        time_t a = mktime(&tm);
        if (tm.tm_mon != mon) break;   /* past the month's end */
        tm.tm_mday += 1; tm.tm_isdst = -1;
        time_t b = mktime(&tm);
        if (e->start < b && end > a) p->mask |= 1u << d;
    }
}

/* days since 1970-01-01 for a civil date (Howard Hinnant's algorithm) */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t cal_parse_time(const char *s, bool *all_day)
{
    int Y, M, D, h = 0, mi = 0, sec = 0, used = 0;
    *all_day = false;
    if (sscanf(s, "%4d-%2d-%2d%n", &Y, &M, &D, &used) != 3 || M < 1 || M > 12 || D < 1 || D > 31) return 0;
    if (s[used] != 'T' && s[used] != ' ') {           /* a date: all day, local midnight */
        struct tm tm = { .tm_year = Y - 1900, .tm_mon = M - 1, .tm_mday = D, .tm_isdst = -1 };
        *all_day = true;
        return (int64_t)mktime(&tm);
    }
    const char *t = s + used + 1;
    int n = 0;
    if (sscanf(t, "%2d:%2d%n", &h, &mi, &n) != 2) return 0;
    t += n;
    if (*t == ':' && sscanf(t + 1, "%2d%n", &sec, &n) == 1) t += 1 + n;
    while (*t == '.' || (*t >= '0' && *t <= '9')) t++;   /* fractions */
    if (*t == 'Z') return days_from_civil(Y, M, D) * 86400 + h * 3600 + mi * 60 + sec;
    int oh, om;
    if ((*t == '+' || *t == '-') && sscanf(t + 1, "%2d:%2d", &oh, &om) == 2) {
        int64_t off = (oh * 3600 + om * 60) * (*t == '-' ? -1 : 1);
        return days_from_civil(Y, M, D) * 86400 + h * 3600 + mi * 60 + sec - off;
    }
    struct tm tm = { .tm_year = Y - 1900, .tm_mon = M - 1, .tm_mday = D, .tm_hour = h, .tm_min = mi,
                     .tm_sec = sec, .tm_isdst = -1 };
    return (int64_t)mktime(&tm);                      /* no offset: local time */
}

/* Drop a UTF-8 sequence cut off at the end of s */
static void utf8_cut(char *s)
{
    int n = strlen(s), i = n;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
    if (i == 0) return;
    unsigned char lead = s[i - 1];
    int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - (i - 1) < need) s[i - 1] = 0;
}

static void put(cal_parse_t *p, char c)
{
    if (p->vlen < (int)sizeof(p->val) - 1) p->val[p->vlen++] = c;
    else p->vtrunc = true;
}

static void put_cp(cal_parse_t *p, uint32_t c)
{
    if (c >= 0xD800 && c <= 0xDFFF) c = '?';          /* surrogate halves: not worth joining */
    if (c < 0x80) put(p, c);
    else if (c < 0x800) { put(p, 0xC0 | c >> 6); put(p, 0x80 | (c & 0x3F)); }
    else { put(p, 0xE0 | c >> 12); put(p, 0x80 | ((c >> 6) & 0x3F)); put(p, 0x80 | (c & 0x3F)); }
}

static void keep_event(cal_parse_t *p)
{
    cal_event_t *e = &p->cur;
    if (!e->start) return;
    if (!e->summary[0]) snprintf(e->summary, sizeof(e->summary), "(no title)");
    mark_days(p, e);
    int k = p->n;
    while (k > 0 && p->ev[k - 1].start > e->start) k--;   /* stable: equal starts keep order */
    if (k >= CAL_MAX_EVENTS) return;
    int last = p->n < CAL_MAX_EVENTS ? p->n : CAL_MAX_EVENTS - 1;
    memmove(&p->ev[k + 1], &p->ev[k], (last - k) * sizeof(*e));
    p->ev[k] = *e;
    if (p->n < CAL_MAX_EVENTS) p->n++;
}

/* A string value of the event's own keys finished */
static void value_done(cal_parse_t *p)
{
    p->val[p->vlen] = 0;
    bool ad;
    if (!strcmp(p->key, "summary")) {
        size_t n = strlen(p->val);
        if (n >= sizeof(p->cur.summary)) n = sizeof(p->cur.summary) - 1;
        memcpy(p->cur.summary, p->val, n);
        p->cur.summary[n] = 0;
        if (p->vtrunc || strlen(p->val) >= sizeof(p->cur.summary)) utf8_cut(p->cur.summary);
    } else if (!strcmp(p->key, "start")) {
        p->cur.start = cal_parse_time(p->val, &ad);
        p->cur.all_day = ad;
    } else if (!strcmp(p->key, "end")) {
        p->cur.end = cal_parse_time(p->val, &ad);
    }
}

void cal_parse_feed(cal_parse_t *p, const char *data, int n)
{
    for (int i = 0; i < n; i++) {
        char c = data[i];
        if (p->mode == 0) {                            /* match the key across chunk borders */
            p->match = c == KEY[p->match] ? p->match + 1 : (c == KEY[0]);
            if (c == '"') {                            /* remember the last two strings */
                if (p->s0) p->last[p->llen] = 0;               /* closing */
                else { memcpy(p->prev, p->last, sizeof(p->prev)); p->llen = 0; }   /* opening */
                p->s0 = !p->s0;
            } else if (p->s0 && p->llen < (int)sizeof(p->last) - 1) {
                p->last[p->llen++] = c;
            }
            if (p->match == (int)sizeof(KEY) - 1) {
                p->mode = 1; p->match = 0; p->s0 = false;
                /* "calendar.x":{"events":[ : prev holds "events", the id came before it */
                p->src = id_index(p->ids, p->prev);
            }
            continue;
        }
        if (p->mode == 1) {
            if (c == '{') {
                p->mode = 2; p->depth = 1; p->in_str = p->esc = false; p->uni = 0;
                p->want_key = true;
                memset(&p->cur, 0, sizeof(p->cur));
                p->cur.src = p->src;
            } else if (c == ']') {
                p->mode = 0;                           /* the next calendar's list may follow */
            }
            continue;
        }
        /* in an event: only the depth-1 keys start, end, summary are read; strings may hold
         * any of {}[]," so they are tracked with their escapes */
        bool collect = p->depth == 1;
        if (p->in_str) {
            if (p->uni) {
                int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                      : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0;
                p->cp = p->cp << 4 | v;
                if (--p->uni == 0 && collect) put_cp(p, p->cp);
                continue;
            }
            if (p->esc) {
                p->esc = false;
                if (c == 'u') { p->uni = 4; p->cp = 0; continue; }
                if (collect) put(p, c == 'n' || c == 't' || c == 'r' ? ' ' : c == 'b' || c == 'f' ? ' ' : c);
                continue;
            }
            if (c == '\\') { p->esc = true; continue; }
            if (c == '"') {
                p->in_str = false;
                if (!collect) continue;
                p->val[p->vlen] = 0;
                if (p->want_key) {
                    size_t k = p->vtrunc ? 0 : strlen(p->val);
                    if (k >= sizeof(p->key)) k = 0;
                    memcpy(p->key, p->val, k);
                    p->key[k] = 0;
                    p->want_key = false;
                } else {
                    value_done(p);
                }
                continue;
            }
            if (collect) {
                if (p->want_key && p->vlen >= (int)sizeof(p->key) - 1) p->vtrunc = true;
                else put(p, c);
            }
            continue;
        }
        if (c == '"') { p->in_str = true; p->vlen = 0; p->vtrunc = false; continue; }
        if (c == '{' || c == '[') { p->depth++; continue; }
        if (c == '}' || c == ']') {
            if (--p->depth == 0) { keep_event(p); p->mode = 1; }
            continue;
        }
        if (c == ',' && p->depth == 1) { p->want_key = true; p->key[0] = 0; }
    }
}
