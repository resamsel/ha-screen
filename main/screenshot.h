/* Screenshots over the USB cable (see screenshot.c, tools/screenshot.py) */
#pragma once
void screenshot_start(void);   /* after LVGL is up */
/* "S" instead of "s": calls this first (LVGL lock held), e.g. to show what a touch shows */
void screenshot_set_touch_cb(void (*cb)(void));
/* "0".."9": switch to that page (LVGL lock held) */
void screenshot_set_page_cb(void (*cb)(int page));
/* "T<x>,<y>\n": a tap at that screen point (LVGL lock held), for testing */
void screenshot_set_tap_cb(void (*cb)(int x, int y));
