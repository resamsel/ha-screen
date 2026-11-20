/* Screenshots over the USB cable: send "s" to the board's USB serial port and it redraws the
 * whole screen once, writing every flushed area as text lines:
 *   SHOT <w> <h>                start
 *   AREA <x1> <y1> <x2> <y2>    followed by one hex line per row (RGB565 as LVGL stores it)
 *   SHOT END
 * tools/screenshot.py turns that into a PNG. There is no full frame buffer in RAM. */
#include "screenshot.h"
#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

/* straight to the USB driver: the console path is too slow for ~220 KB of hex */
static void out(const char *t, int n) { usb_serial_jtag_write_bytes(t, n, portMAX_DELAY); }

static void (*s_flush)(lv_disp_drv_t *, const lv_area_t *, lv_color_t *);

static void dump_flush(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px)
{
    static char line[320 * 4 + 2];
    static const char hex[] = "0123456789abcdef";
    int w = lv_area_get_width(a);
    char hdr[48];
    out(hdr, snprintf(hdr, sizeof(hdr), "AREA %d %d %d %d\n", a->x1, a->y1, a->x2, a->y2));
    const uint8_t *p = (const uint8_t *)px;
    for (int y = a->y1; y <= a->y2; y++) {
        int k = 0;
        for (int x = 0; x < w * 2; x++, p++) { line[k++] = hex[*p >> 4]; line[k++] = hex[*p & 15]; }
        line[k++] = '\n';
        out(line, k);
    }
    s_flush(drv, a, px);
}

static void (*s_touch)(void);
static void (*s_page)(int);
static void (*s_tap)(int, int);
void screenshot_set_tap_cb(void (*cb)(int, int)) { s_tap = cb; }
void screenshot_set_page_cb(void (*cb)(int)) { s_page = cb; }
void screenshot_set_touch_cb(void (*cb)(void)) { s_touch = cb; }

static void take(bool touch)
{
    if (touch && s_touch && lvgl_port_lock(0)) {
        s_touch();
        lvgl_port_unlock();
        vTaskDelay(pdMS_TO_TICKS(400));   /* let the fade-in finish */
    }
    if (!lvgl_port_lock(0)) return;
    lv_disp_t *d = lv_disp_get_default();
    if (d) {
        s_flush = d->driver->flush_cb;
        d->driver->flush_cb = dump_flush;
        char hdr[32];
        out(hdr, snprintf(hdr, sizeof(hdr), "SHOT %d %d\n", (int)lv_disp_get_hor_res(d), (int)lv_disp_get_ver_res(d)));
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(d);
        d->driver->flush_cb = s_flush;
        out("SHOT END\n", 9);
    }
    lvgl_port_unlock();
}

static void task(void *arg)
{
    (void)arg;
    uint8_t c;
    for (;;)
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) continue;
        else if (c == 's' || c == 'S') take(c == 'S');
        else if (c >= '0' && c <= '9' && s_page && lvgl_port_lock(0)) { s_page(c - '0'); lvgl_port_unlock(); }
        else if (c == 'T') {
            char buf[16];
            int n = 0;
            while (n < (int)sizeof(buf) - 1 && usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(500)) == 1 && c != '\n') buf[n++] = c;
            buf[n] = 0;
            int x, y;
            if (s_tap && sscanf(buf, "%d,%d", &x, &y) == 2 && lvgl_port_lock(0)) { s_tap(x, y); lvgl_port_unlock(); }
        }
}

void screenshot_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 2048;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) return;
    xTaskCreate(task, "shot", 3072, NULL, 2, NULL);
}
