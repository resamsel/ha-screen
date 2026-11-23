/* Host tests for main/notify.c. Build/run: see test/run.sh */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "notify.h"

int main(void)
{
    char o[NOTIFY_MAX];
    assert(notify_text("Washing machine done", o, sizeof(o)) && !strcmp(o, "Washing machine done"));
    assert(!notify_text("", o, sizeof(o)));
    assert(!notify_text("unknown", o, sizeof(o)));
    assert(!notify_text("unavailable", o, sizeof(o)));
    assert(!notify_text(NULL, o, sizeof(o)));
    assert(notify_text("Say \"hi\"\nnow", o, sizeof(o)) && !strcmp(o, "Say \"hi\"\nnow"));
    char big[256];
    memset(big, 'x', 255); big[255] = 0;
    assert(notify_text(big, o, sizeof(o)) && strlen(o) == 255);
    char s6[6];
    assert(notify_text("T\xc3\xbcr offen", s6, sizeof(s6)) && !strcmp(s6, "T\xc3\xbcr "));
    char s3[3];   /* "T" + half of ü would be 2 bytes: cut before ü */
    assert(notify_text("T\xc3\xbcr", s3, sizeof(s3)) && !strcmp(s3, "T"));
    /* resyncs re-deliver the helper's state: show each message once until HA empties it */
    notify_seen_t seen = {0};
    assert(notify_new(&seen, "A", o, sizeof(o)) && !strcmp(o, "A"));
    assert(!notify_new(&seen, "A", o, sizeof(o)));     /* resync snapshot: same message */
    assert(notify_new(&seen, "B", o, sizeof(o)));      /* a different one shows */
    assert(!notify_new(&seen, "", o, sizeof(o)));      /* cleared: nothing to show... */
    assert(notify_new(&seen, "B", o, sizeof(o)));      /* ...and the same text shows again */
    assert(!notify_new(&seen, "unknown", o, sizeof(o)));
    assert(!notify_new(&seen, NULL, o, sizeof(o)));
    puts("notify: ok");
    return 0;
}
