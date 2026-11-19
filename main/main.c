#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"

#include "nvs_flash.h"

#include "esp_lvgl_port.h"

#include "bsp_display.h"
#include "bsp_touch.h"
#include "bsp_i2c.h"
#include "bsp_spi.h"
#include "ha_client.h"
#include "driver/gpio.h"
static void button_init(void);
#include "config.h"
// #include "bsp_wifi.h"
// #include "bsp_sdcard.h"
// #include "bsp_battery.h"
// #include "bsp_qmi8658.h"

// #include "iot_button.h"
// #include "button_gpio.h"

#include "demos/lv_demos.h"


// #include "lvgl_ui.h"

#define EXAMPLE_DISPLAY_ROTATION 0

#if EXAMPLE_DISPLAY_ROTATION == 90 || EXAMPLE_DISPLAY_ROTATION == 270
#define EXAMPLE_LCD_H_RES (320)
#define EXAMPLE_LCD_V_RES (172)
#else
#define EXAMPLE_LCD_H_RES (172)
#define EXAMPLE_LCD_V_RES (320)
#endif

#define EXAMPLE_LCD_DRAW_BUFF_HEIGHT (50)
#define EXAMPLE_LCD_DRAW_BUFF_DOUBLE (1)


static char *TAG = "lvgl_example";

/* LCD IO and panel */
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_touch_handle_t touch_handle = NULL;

/* LVGL display and touch */
static lv_display_t *lvgl_disp = NULL;
static lv_indev_t *lvgl_touch_indev = NULL;


static esp_err_t app_lvgl_init(void);
static void create_ui(void);
static void ha_toggle_cb(lv_event_t *e);
static void set_busy(bool busy);
static void show_status(ha_state_t state);
static void apply_panel_theme(bool light);
static void ha_sensor_changed(int page, int row, const char *value, const char *unit);
static void axis_text(char *b, size_t n, float v, const char *unit)
{
    if (v >= 10 || v == floorf(v)) snprintf(b, n, "%.0f %s", (double)v, unit);
    else snprintf(b, n, "%.1f %s", (double)v, unit);
}

static void ha_graph_changed(int graph, const char *latest, const char *unit, const float *pts, int n, int64_t end);
static void place_graph_value(void);
static void place_time_axis(int64_t end);
static float nice_ceil(float x);
static void ha_state_changed(ha_state_t state);
static void ha_brightness_changed(float value, float min, float max);
static void slider_cb(lv_event_t *e);
static void ha_update_done(bool ok);
static void ha_errors(const char *errors);
static bool s_busy = false;
static bool s_swiped = false;   /* this touch became a swipe: buttons ignore its release */
static bool s_light = false;   /* current theme */
static bool s_refreshing = false;
static lv_obj_t *s_refresh_btn = NULL;
static void refresh_cb(lv_event_t *e);
static lv_color_t neutral_btn_color(void);
/* Status icons: [0] on the main screen, [1] on the config panel */
static lv_obj_t *s_wifi_icon[2];
static lv_obj_t *s_ha_icon[2];

/* Wi-Fi symbol + HA dot in the top right corner of `parent` */
static void create_status_icons(lv_obj_t *parent, int set)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -10, 14);
    lv_obj_t *wifi = lv_label_create(parent);
    lv_label_set_text(wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi, &lv_font_montserrat_14, 0);
    lv_obj_align_to(wifi, dot, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    s_ha_icon[set] = dot;
    s_wifi_icon[set] = wifi;
}
static lv_obj_t *s_slider = NULL;
static lv_obj_t *s_orient_btn = NULL;
static bool s_landscape = false;   /* orientation, kept in NVS */
static void orient_cb(lv_event_t *e);
static void apply_rotation(void);
static void ha_landscape_changed(bool landscape);
static lv_obj_t *s_ha_btn = NULL;


void app_main(void)
{
    i2c_master_bus_handle_t i2c_bus_handle;
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    i2c_bus_handle = bsp_i2c_init();
    bsp_spi_init();
    bsp_display_init(&io_handle, &panel_handle, EXAMPLE_LCD_H_RES * EXAMPLE_LCD_DRAW_BUFF_HEIGHT);
    bsp_touch_init(&touch_handle, i2c_bus_handle, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, EXAMPLE_DISPLAY_ROTATION);

    ESP_ERROR_CHECK(app_lvgl_init());



    bsp_display_brightness_init();
    bsp_display_set_brightness(100);

    if (lvgl_port_lock(0))
    {
        // lv_demo_benchmark();
        // lv_demo_music();
        nvs_handle_t nv;
        uint8_t land = 0;
        if (nvs_open("ui", NVS_READONLY, &nv) == ESP_OK) { nvs_get_u8(nv, "landscape", &land); nvs_close(nv); }
        s_landscape = land;
        apply_rotation();
        create_ui();
        button_init();
        lvgl_port_unlock();
    }

    setenv("TZ", HA_TIMEZONE, 1);   /* for local times on the graph axis */
    tzset();
    ha_client_set_error_cb(ha_errors);
    ha_client_set_landscape_cb(ha_landscape_changed);
    ha_client_start(ha_state_changed, ha_brightness_changed, ha_update_done, ha_sensor_changed, ha_graph_changed);
}

#include "config.h"
#include "nvs.h"

/* Screen size in the current orientation (portrait 172x320, landscape 320x172) */
#define SCR_W  lv_disp_get_hor_res(NULL)
#define SCR_H  lv_disp_get_ver_res(NULL)
/* value / unit / name fonts: smaller in landscape, where three values share one row */
#define VFONT  (s_landscape ? &lv_font_montserrat_28 : &lv_font_montserrat_36)
#define UFONT  (s_landscape ? &lv_font_montserrat_16 : &lv_font_montserrat_22)
#define NFONT  (s_landscape ? &lv_font_montserrat_14 : &lv_font_montserrat_16)
#define NAME_DY (s_landscape ? 34 : 44)       /* name label below the value's top */
#define GRAPH_LW 112                          /* landscape graph page: value column width */

/* ---- Main screen: pages dragged left/right, with page dots at the bottom ----------
 * Sensor pages (temperature, humidity) first, then the graph page.
 * Rows share the height between the status line and the dots evenly. */
#define STATUS_H   30                    /* top strip reserved for Wi-Fi icon and HA dot */
#define DOTS_H     22                    /* bottom strip for the page dots */
#define ROW_BLOCK  64                    /* value (36 px font) + name below it */
#define PAGE_W     SCR_W
#define SWIPE_MIN  40                    /* drag distance that flips the page */
#define PAGE_COUNT (HA_SENSOR_PAGES + HA_GRAPH_COUNT)
#define IS_GRAPH(pg) ((pg) >= HA_SENSOR_PAGES)        /* graph pages come last */

/* Where row i's value goes: portrait = rows stacked, landscape = side-by-side columns.
 * cx = horizontal centre, y = top of the value. */
static void value_pos(int i, lv_coord_t *cx, lv_coord_t *y);
static lv_coord_t row_y(int i)
{
    lv_coord_t slot = (SCR_H - STATUS_H - DOTS_H) / HA_ROW_COUNT;
    return STATUS_H + i * slot + (slot - ROW_BLOCK) / 2;
}

static lv_obj_t *s_strip = NULL;                         /* all pages side by side */
static lv_obj_t *s_name[HA_SENSOR_PAGES][HA_ROW_COUNT];
static lv_obj_t *s_val[HA_SENSOR_PAGES][HA_ROW_COUNT];
static lv_obj_t *s_unit[HA_SENSOR_PAGES][HA_ROW_COUNT];
static lv_obj_t *s_dot[PAGE_COUNT];
static int s_page = 0;
static lv_obj_t *s_page_title = NULL;               /* page name, left side of the status line */

/* graph page */
/* graph pages: one set per graph; s_gi selects which one the s_g_* / s_chart names mean */
static int s_gi = 0;
static lv_obj_t *s_g_name_[HA_GRAPH_COUNT], *s_g_val_[HA_GRAPH_COUNT], *s_g_unit_[HA_GRAPH_COUNT], *s_chart_[HA_GRAPH_COUNT];
static lv_chart_series_t *s_chart_ser_[HA_GRAPH_COUNT];
#define s_g_name    s_g_name_[s_gi]
#define s_g_val     s_g_val_[s_gi]
#define s_g_unit    s_g_unit_[s_gi]
#define s_chart     s_chart_[s_gi]
#define s_chart_ser s_chart_ser_[s_gi]
#define AXIS_MAX  (HA_GRAPH_HOURS / HA_GRAPH_TICK_HOURS + 1)
#define AXIS_H    16
static lv_obj_t *s_axis_[HA_GRAPH_COUNT][AXIS_MAX];
static float s_ytop_[HA_GRAPH_COUNT], s_ybot_[HA_GRAPH_COUNT];
static lv_obj_t *s_ymin_[HA_GRAPH_COUNT];   /* y axis lower bound, bottom-left (if not 0) */
#define s_ybot s_ybot_[s_gi]
#define s_ymin s_ymin_[s_gi]
static lv_obj_t *s_ymax_[HA_GRAPH_COUNT];   /* y axis top value, top-left of the graph */
#define s_axis s_axis_[s_gi]
#define s_ytop s_ytop_[s_gi]
#define s_ymax s_ymax_[s_gi]


/* Centre the value horizontally in its row and hang the unit off its right side,
 * on the same baseline. Call after changing either text (LVGL lock held). */
static void value_pos(int i, lv_coord_t *cx, lv_coord_t *y)
{
    if (s_landscape) {
        lv_coord_t colw = SCR_W / HA_ROW_COUNT;
        *cx = i * colw + colw / 2;
        *y = STATUS_H + (SCR_H - STATUS_H - DOTS_H - 52) / 2;
    } else {
        *cx = SCR_W / 2;
        *y = row_y(i);
    }
}

/* Value centred on cx with the unit hanging off its right side, same baseline */
static void page_relayout(int pg);
static void place_pair(lv_obj_t *val, lv_obj_t *unit, lv_coord_t cx, lv_coord_t y)
{
    /* align on the untranslated positions; page_relayout re-applies the active shifts */
    lv_obj_set_style_translate_x(val, 0, 0);
    lv_obj_set_style_translate_y(val, 0, 0);
    lv_obj_set_style_translate_x(unit, 0, 0);
    lv_obj_set_style_translate_y(unit, 0, 0);
    lv_obj_align(val, LV_ALIGN_TOP_MID, cx - SCR_W / 2, y);
    lv_obj_update_layout(val);
    lv_obj_align_to(unit, val, LV_ALIGN_OUT_RIGHT_BOTTOM, 3,
                    -(((const lv_font_t *)VFONT)->base_line - ((const lv_font_t *)UFONT)->base_line));
}

static void place_value(int pg, int row)
{
    lv_coord_t cx, y;
    value_pos(row, &cx, &y);
    place_pair(s_val[pg][row], s_unit[pg][row], cx, y);
    if (s_unit[pg][HA_ROW_COUNT - 1]) page_relayout(pg);   /* once the page is complete */
}

/* Sensor text and page dot colours for the current theme. LVGL lock must be held. */
static void apply_temp_theme(bool light)
{
    lv_color_t fg = light ? lv_color_hex(0x1C1C1E) : lv_color_white();
    lv_color_t sub = lv_color_hex(light ? 0x4A5058 : 0x9AA0A8);
    for (int pg = 0; pg < HA_SENSOR_PAGES; pg++) {
        for (int i = 0; i < HA_ROW_COUNT; i++) {
            if (!s_val[pg][i]) continue;
            lv_obj_set_style_text_color(s_name[pg][i], sub, 0);
            lv_obj_set_style_text_color(s_val[pg][i], fg, 0);
            lv_obj_set_style_text_color(s_unit[pg][i], fg, 0);
        }
    }
    for (s_gi = 0; s_gi < HA_GRAPH_COUNT; s_gi++) {
        if (!s_chart) continue;
        for (int i = 0; i < AXIS_MAX; i++) lv_obj_set_style_text_color(s_axis[i], sub, 0);
        lv_obj_set_style_text_color(s_ymax, sub, 0);
        lv_obj_set_style_text_color(s_ymin, sub, 0);
        lv_obj_set_style_text_color(s_g_name, sub, 0);
        lv_obj_set_style_text_color(s_g_val, fg, 0);
        lv_obj_set_style_text_color(s_g_unit, fg, 0);
        lv_chart_set_series_color(s_chart, s_chart_ser, lv_color_hex(light ? 0x0B7FAB : 0x18BCF2));
    }
    s_gi = 0;
    if (s_page_title) lv_obj_set_style_text_color(s_page_title, fg, 0);
    for (int pg = 0; pg < PAGE_COUNT; pg++) {
        if (!s_dot[pg]) continue;
        lv_obj_set_style_bg_color(s_dot[pg], pg == s_page ? fg : lv_color_hex(light ? 0xAEB4BE : 0x4A5058), 0);
    }
}

static void strip_anim_x(void *obj, int32_t x) { lv_obj_set_x(obj, x); }

static void value_select(int g);
static void show_page(int page)
{
    value_select(-1);
    static const char *titles[PAGE_COUNT] = HA_PAGE_TITLES;
    if (page < 0) page = 0;                       /* no wrap-around: stop at either end */
    if (page >= PAGE_COUNT) page = PAGE_COUNT - 1;
    s_page = page;
    lv_label_set_text(s_page_title, titles[s_page]);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_strip);
    lv_anim_set_exec_cb(&a, strip_anim_x);
    lv_anim_set_values(&a, lv_obj_get_x(s_strip), -s_page * PAGE_W);
    lv_anim_set_time(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    apply_temp_theme(s_light);
}

/* Network task: new value for one sensor */
static void ha_sensor_changed(int page, int row, const char *value, const char *unit)
{
    if (page < 0 || page >= HA_SENSOR_PAGES || row < 0 || row >= HA_ROW_COUNT || !lvgl_port_lock(0)) return;
    lv_label_set_text(s_val[page][row], value);
    lv_label_set_text(s_unit[page][row], unit);
    place_value(page, row);
    lvgl_port_unlock();
}

/* Network task: graph page value, or a fresh set of history points */
static void ha_graph_changed(int graph, const char *latest, const char *unit, const float *pts, int n, int64_t end)
{
    if (graph < 0 || graph >= HA_GRAPH_COUNT || !lvgl_port_lock(0)) return;
    s_gi = graph;
    if (!s_chart) { lvgl_port_unlock(); return; }
    if (latest) {
        lv_label_set_text(s_g_val, latest);
        lv_label_set_text(s_g_unit, unit ? unit : "");
        place_graph_value();
    }
    if (pts) {
        /* Gaussian smoothing, width HA_GRAPH_SMOOTHING points (0 = none); keeps short spikes visible */
#define SMOOTH_MAX 30
        const int SMOOTH_R = LV_MIN((int)(HA_GRAPH_SMOOTHING * 3 + 0.999f), SMOOTH_MAX);  /* reaches 3 sigma */
        static float w[2 * SMOOTH_MAX + 1];
        if (w[SMOOTH_R] == 0)
            for (int k = -SMOOTH_R; k <= SMOOTH_R; k++)
                w[k + SMOOTH_R] = HA_GRAPH_SMOOTHING > 0
                    ? expf(-(float)(k * k) / (2 * HA_GRAPH_SMOOTHING * HA_GRAPH_SMOOTHING)) : 1;
        static float sm[HA_GRAPH_POINTS];
        float mx = -INFINITY, mn = INFINITY;
        for (int i = 0; i < n && i < HA_GRAPH_POINTS; i++) {
            sm[i] = NAN;
            if (isnan(pts[i])) continue;
            float sum = 0, ws = 0;
            for (int k = -SMOOTH_R; k <= SMOOTH_R; k++) {
                int j = i + k;
                if (j < 0 || j >= n || isnan(pts[j])) continue;
                sum += w[k + SMOOTH_R] * pts[j];
                ws += w[k + SMOOTH_R];
            }
            sm[i] = sum / ws;
            if (sm[i] > mx) mx = sm[i];
            if (sm[i] < mn) mn = sm[i];
        }
        if (isinf(mx)) { mx = 1; mn = 0; }      /* no data yet */
        static const int zero_based[HA_GRAPH_COUNT] = HA_GRAPH_ZERO_BASED;
        float top, bot = 0;
        if (zero_based[s_gi]) {
            /* 0 .. the next "nice" number (1, 2, 2.5, 5 x 10^n) above the peak */
            top = nice_ceil(LV_MAX(mx, 1.0f) * 1.05f);
        } else {
            /* data range rounded out to a nice step (about a quarter of the range), so
             * the line fills the chart instead of hugging the top */
            float step = nice_ceil(LV_MAX(mx - mn, 0.4f) / 4);
            bot = floorf(mn / step) * step;
            top = ceilf(mx / step) * step;
            if (top - mx < step * 0.1f) top += step;    /* a little headroom above the peak */
            if (bot < 0) bot = 0;
        }
        lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, (lv_coord_t)(bot * 10), (lv_coord_t)(top * 10));
        s_ytop = top;
        s_ybot = bot;
        for (int i = 0; i < n && i < HA_GRAPH_POINTS; i++)
            lv_chart_set_value_by_id(s_chart, s_chart_ser, i,
                                     isnan(sm[i]) ? LV_CHART_POINT_NONE : (lv_coord_t)(sm[i] * 10));
        lv_chart_refresh(s_chart);
        place_time_axis(end);
    }
    if (s_ytop > 0) {           /* "10 %": axis values plus the sensor's unit */
        char tb[24];
        const char *un = lv_label_get_text(s_g_unit);
        axis_text(tb, sizeof(tb), s_ytop, un);
        lv_label_set_text(s_ymax, tb);
        if (s_ybot > 0) {       /* lower bound only when the axis doesn't start at 0 */
            axis_text(tb, sizeof(tb), s_ybot, un);
            lv_label_set_text(s_ymin, tb);
            lv_obj_clear_flag(s_ymin, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ymin, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lvgl_port_unlock();
}

/* Time labels under the graph at every HA_GRAPH_TICK_HOURS of local clock time
 * (e.g. 18:00, 0:00, 6:00, 12:00), positioned to match the chart's x axis. */

static float nice_ceil(float x)
{
    if (x <= 0) return 1;
    float mag = powf(10, floorf(log10f(x)));
    static const float steps[] = {1, 2, 2.5f, 5, 10};
    for (int i = 0; i < 5; i++) if (steps[i] * mag >= x) return steps[i] * mag;
    return 10 * mag;
}

static void place_time_axis(int64_t end)
{
    if (!end) return;
    const int64_t span = HA_GRAPH_HOURS * 3600, start = end - span;
    const lv_coord_t pad = 2, w = lv_obj_get_width(s_chart) - 2 * pad;
    const lv_coord_t x0 = lv_obj_get_x(s_chart) + pad;
    const lv_coord_t y = lv_obj_get_y(s_chart) + lv_obj_get_height(s_chart) + 1;
    int used = 0;
    /* walk whole hours from the left edge; label those on a tick boundary */
    for (int64_t t = (start / 3600 + 1) * 3600; t < end && used < AXIS_MAX; t += 3600) {
        time_t tt = (time_t)t;
        struct tm lt;
        localtime_r(&tt, &lt);
        if (lt.tm_hour % HA_GRAPH_TICK_HOURS) continue;
        lv_obj_t *l = s_axis[used++];
        lv_label_set_text_fmt(l, "%d:00", lt.tm_hour);
        lv_obj_update_layout(l);
        lv_coord_t lw = lv_obj_get_width(l);
        lv_coord_t x = x0 + (lv_coord_t)((t - start) * w / span) - lw / 2;
        if (x < 0) x = 0;                                  /* keep labels on screen */
        if (x + lw > PAGE_W) x = PAGE_W - lw;
        lv_obj_set_pos(l, x, y);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = used; i < AXIS_MAX; i++) lv_obj_add_flag(s_axis[i], LV_OBJ_FLAG_HIDDEN);
}

/* Graph page value: portrait = on top like a sensor row, landscape = left column */
static void graph_value_pos(lv_coord_t *cx, lv_coord_t *y)
{
    if (s_landscape) {
        *cx = GRAPH_LW / 2;
        *y = STATUS_H + (SCR_H - STATUS_H - DOTS_H - 52) / 2;
    } else {
        *cx = SCR_W / 2;
        *y = row_y(0);
    }
}

static void place_graph_value(void)
{
    lv_coord_t cx, y;
    graph_value_pos(&cx, &y);
    place_pair(s_g_val, s_g_unit, cx, y);
    if (s_g_name) page_relayout(HA_SENSOR_PAGES + s_gi);
}

static lv_obj_t *make_page(int pg)
{
    lv_obj_t *page = lv_obj_create(s_strip);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, PAGE_W, SCR_H);
    lv_obj_set_pos(page, pg * PAGE_W, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

static void create_graph_page(lv_obj_t *page)
{
    /* same layout as the sensor rows: value centred, unit beside it, name below */
    lv_coord_t cx, y;
    graph_value_pos(&cx, &y);
    s_g_val = lv_label_create(page);
    lv_label_set_text(s_g_val, "--");
    lv_obj_set_style_text_font(s_g_val, VFONT, 0);
    s_g_unit = lv_label_create(page);
    lv_label_set_text(s_g_unit, "");
    lv_obj_set_style_text_font(s_g_unit, UFONT, 0);
    lv_obj_set_style_text_opa(s_g_unit, LV_OPA_80, 0);
    place_graph_value();
    s_g_name = lv_label_create(page);
    static const char *gnames[HA_GRAPH_COUNT] = HA_GRAPH_NAMES;
    lv_label_set_text(s_g_name, gnames[s_gi]);
    lv_obj_set_style_text_font(s_g_name, NFONT, 0);
    lv_obj_align(s_g_name, LV_ALIGN_TOP_MID, cx - SCR_W / 2, y + NAME_DY);

    /* line chart filling the lower part of the page, no axes or grid */
    s_chart = lv_chart_create(page);
    if (s_landscape) {   /* right of the value column, full height */
        lv_obj_set_size(s_chart, SCR_W - GRAPH_LW - 4, SCR_H - DOTS_H - AXIS_H - STATUS_H - 4);
        lv_obj_set_pos(s_chart, GRAPH_LW, STATUS_H + 4);
    } else {             /* below the value */
        lv_coord_t h = SCR_H - DOTS_H - AXIS_H - (y + ROW_BLOCK + 16);
        lv_obj_set_size(s_chart, PAGE_W, h);
        lv_obj_set_pos(s_chart, 0, SCR_H - DOTS_H - AXIS_H - h);
    }
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_radius(s_chart, 0, 0);
    lv_obj_set_style_pad_all(s_chart, 2, 0);
    lv_chart_set_div_line_count(s_chart, 0, 0);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, HA_GRAPH_POINTS);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 10);
    lv_obj_set_style_line_width(s_chart, HA_GRAPH_LINE_WIDTH, LV_PART_ITEMS);
    lv_obj_set_style_line_rounded(s_chart, true, LV_PART_ITEMS);   /* round joints */
    lv_obj_set_style_size(s_chart, 0, LV_PART_INDICATOR);          /* no point markers */
    s_chart_ser = lv_chart_add_series(s_chart, lv_color_hex(0x18BCF2), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(s_chart, s_chart_ser, LV_CHART_POINT_NONE);

    s_ymax = lv_label_create(page);
    lv_label_set_text(s_ymax, "");
    lv_obj_set_style_text_font(s_ymax, &lv_font_montserrat_12, 0);
    lv_obj_align_to(s_ymax, s_chart, LV_ALIGN_TOP_LEFT, 4, 0);
    s_ymin = lv_label_create(page);
    lv_label_set_text(s_ymin, "");
    lv_obj_set_style_text_font(s_ymin, &lv_font_montserrat_12, 0);
    lv_obj_align_to(s_ymin, s_chart, LV_ALIGN_BOTTOM_LEFT, 4, 0);
    lv_obj_add_flag(s_ymin, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < AXIS_MAX; i++) {
        s_axis[i] = lv_label_create(page);
        lv_obj_set_style_text_font(s_axis[i], &lv_font_montserrat_12, 0);
        lv_obj_add_flag(s_axis[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void create_pages(lv_obj_t *scr)
{
    static const char *names[HA_ROW_COUNT] = HA_ROW_NAMES;

    s_strip = lv_obj_create(scr);
    lv_obj_remove_style_all(s_strip);
    lv_obj_set_size(s_strip, PAGE_W * PAGE_COUNT, SCR_H);
    lv_obj_set_pos(s_strip, 0, 0);
    lv_obj_clear_flag(s_strip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    for (int pg = 0; pg < HA_SENSOR_PAGES; pg++) {
        lv_obj_t *page = make_page(pg);
        for (int i = 0; i < HA_ROW_COUNT; i++) {
            lv_coord_t cx, y;
            value_pos(i, &cx, &y);
            s_name[pg][i] = lv_label_create(page);
            lv_label_set_text(s_name[pg][i], names[i]);
            lv_obj_set_style_text_font(s_name[pg][i], NFONT, 0);
            lv_obj_align(s_name[pg][i], LV_ALIGN_TOP_MID, cx - SCR_W / 2, y + NAME_DY);
            /* value centred on the page; unit placed right next to it */
            s_val[pg][i] = lv_label_create(page);
            lv_label_set_text(s_val[pg][i], "--");
            lv_obj_set_style_text_font(s_val[pg][i], VFONT, 0);
            s_unit[pg][i] = lv_label_create(page);
            lv_label_set_text(s_unit[pg][i], "");
            lv_obj_set_style_text_font(s_unit[pg][i], UFONT, 0);
            lv_obj_set_style_text_opa(s_unit[pg][i], LV_OPA_80, 0);
            place_value(pg, i);
        }
    }
    for (s_gi = 0; s_gi < HA_GRAPH_COUNT; s_gi++) create_graph_page(make_page(HA_SENSOR_PAGES + s_gi));
    s_gi = 0;

    /* Page dots, centred at the bottom; fixed on the screen, not part of the strip */
    const lv_coord_t d = 8, gap = 10;
    lv_coord_t total = PAGE_COUNT * d + (PAGE_COUNT - 1) * gap;
    for (int pg = 0; pg < PAGE_COUNT; pg++) {
        s_dot[pg] = lv_obj_create(scr);
        lv_obj_remove_style_all(s_dot[pg]);
        lv_obj_set_size(s_dot[pg], d, d);
        lv_obj_set_style_radius(s_dot[pg], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_dot[pg], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_dot[pg], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(s_dot[pg], LV_ALIGN_BOTTOM_LEFT,
                     (PAGE_W - total) / 2 + pg * (d + gap), -(DOTS_H - d) / 2);
    }

    s_page_title = lv_label_create(scr);
    lv_obj_set_style_text_font(s_page_title, &lv_font_montserrat_16, 0);
    /* vertically centred on the status icons (HA dot centre is at y = 14 + 10/2) */
    lv_obj_align(s_page_title, LV_ALIGN_TOP_LEFT, 10, 19 - lv_font_montserrat_16.line_height / 2);
    {
        static const char *titles[PAGE_COUNT] = HA_PAGE_TITLES;
        lv_label_set_text(s_page_title, titles[0]);
    }
    apply_temp_theme(s_light);
}


/* ---- Tapped value grows (only the number; unit and name keep their size and move along) ----------- */
#define GROUPS     (HA_SENSOR_PAGES * HA_ROW_COUNT + HA_GRAPH_COUNT)   /* graph values last */
#define ZOOM_BIG   ((uint16_t)(HA_ACTIVE_SCALE * 256 + 0.5f))   /* LVGL zoom: 256 = 100 % */
static int s_sel = -1;
static uint16_t s_zoom[GROUPS];

static void group_objs(int g, lv_obj_t *o[3])
{
    if (g >= HA_SENSOR_PAGES * HA_ROW_COUNT) {
        s_gi = g - HA_SENSOR_PAGES * HA_ROW_COUNT;
        o[0] = s_g_val; o[1] = s_g_unit; o[2] = s_g_name;
        return;
    }
    int pg = g / HA_ROW_COUNT, row = g % HA_ROW_COUNT;
    o[0] = s_val[pg][row]; o[1] = s_unit[pg][row]; o[2] = s_name[pg][row];
}

static int group_page(int g)
{
    return g >= HA_SENSOR_PAGES * HA_ROW_COUNT ? HA_SENSOR_PAGES + (g - HA_SENSOR_PAGES * HA_ROW_COUNT) : g / HA_ROW_COUNT;
}

/* Screen area covered by a group's first n labels (2 = value and unit, 3 = with name) */
static void group_area_n(int g, int n, lv_area_t *a)
{
    lv_obj_t *o[3];
    group_objs(g, o);
    lv_obj_get_coords(o[0], a);
    for (int i = 1; i < n; i++) {
        lv_area_t b;
        lv_obj_get_coords(o[i], &b);
        if (b.x1 < a->x1) a->x1 = b.x1;
        if (b.y1 < a->y1) a->y1 = b.y1;
        if (b.x2 > a->x2) a->x2 = b.x2;
        if (b.y2 > a->y2) a->y2 = b.y2;
    }
}

static void group_area(int g, lv_area_t *a) { group_area_n(g, 3, a); }

/* Lay out every element on page pg for the current zoom levels. The active number grows
 * about its centre; its unit keeps its size and follows the number's right edge and
 * baseline; its name moves down to keep the gap. Neighbours move apart by the extra size
 * (rows up/down in portrait, columns left/right in landscape), and an element that would
 * cross the screen (or graph) edge is pushed back inside, so nothing gets cut off. */
static void page_relayout(int pg)
{
    int gs[HA_ROW_COUNT], n = 0;
    for (int g = 0; g < GROUPS; g++) if (group_page(g) == pg) gs[n++] = g;
    lv_area_t va[HA_ROW_COUNT], ua[HA_ROW_COUNT], na[HA_ROW_COUNT];
    float f[HA_ROW_COUNT];
    lv_obj_t *o[HA_ROW_COUNT][3];
    for (int k = 0; k < n; k++) {
        group_objs(gs[k], o[k]);
        if (!o[k][0]) return;
        f[k] = ((s_zoom[gs[k]] ? s_zoom[gs[k]] : 256) - 256) / 256.0f;
        lv_area_t *ar[3] = {&va[k], &ua[k], &na[k]};
        for (int i = 0; i < 3; i++) {       /* untranslated positions */
            lv_obj_get_coords(o[k][i], ar[i]);
            lv_area_move(ar[i], -lv_obj_get_style_translate_x(o[k][i], 0), -lv_obj_get_style_translate_y(o[k][i], 0));
        }
    }
    bool cols = s_landscape && !IS_GRAPH(pg);
    lv_coord_t right_bound = (s_landscape && IS_GRAPH(pg)) ? GRAPH_LW - 2 : SCR_W - 2;
    const lv_font_t *vf = VFONT;
    for (int k = 0; k < n; k++) {
        /* push away from the neighbours' growth */
        float shift = 0;
        for (int j = 0; j < n; j++) {
            if (j == k) continue;
            float ext = cols ? lv_area_get_width(&va[j]) * f[j] : lv_area_get_height(&va[j]) * f[j];
            shift += (j < k ? ext : -ext) / 2;
        }
        lv_coord_t tx = cols ? (lv_coord_t)shift : 0, ty = cols ? 0 : (lv_coord_t)shift;
        lv_coord_t w = lv_area_get_width(&va[k]), h = lv_area_get_height(&va[k]);
        lv_coord_t gw = (lv_coord_t)(w * f[k]), gh = (lv_coord_t)(h * f[k]);
        lv_coord_t udx = gw / 2;
        lv_coord_t udy = (lv_coord_t)(((vf->line_height - vf->base_line) - h / 2) * f[k]);
        /* keep the whole element on screen */
        /* page-relative: the page may be anywhere on the strip (even off screen) */
        lv_area_t pa;
        lv_obj_get_coords(lv_obj_get_parent(o[k][0]), &pa);
        lv_coord_t left = LV_MIN(va[k].x1 - gw / 2, na[k].x1) + tx - pa.x1;
        lv_coord_t right = LV_MAX(LV_MAX(va[k].x2 + gw / 2, ua[k].x2 + udx), na[k].x2) + tx - pa.x1;
        if (right > right_bound) tx -= right - right_bound;
        else if (left < 2) tx += 2 - left;
        lv_obj_set_style_transform_pivot_x(o[k][0], w / 2, 0);
        lv_obj_set_style_transform_pivot_y(o[k][0], h / 2, 0);
        lv_obj_set_style_transform_zoom(o[k][0], (lv_coord_t)(256 + f[k] * 256 + 0.5f), 0);
        lv_obj_set_style_translate_x(o[k][0], tx, 0);
        lv_obj_set_style_translate_y(o[k][0], ty, 0);
        lv_obj_set_style_translate_x(o[k][1], tx + udx, 0);
        lv_obj_set_style_translate_y(o[k][1], ty + udy, 0);
        lv_obj_set_style_translate_x(o[k][2], tx, 0);
        lv_obj_set_style_translate_y(o[k][2], ty + gh / 2, 0);
    }
}

static void group_zoom_cb(void *var, int32_t z)
{
    int g = (int)((uint16_t *)var - s_zoom);
    s_zoom[g] = (uint16_t)z;
    page_relayout(group_page(g));
}

static void group_animate(int g, uint16_t to)
{
    lv_anim_del(&s_zoom[g], group_zoom_cb);
    if (!s_zoom[g]) s_zoom[g] = 256;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_zoom[g]);
    lv_anim_set_exec_cb(&a, group_zoom_cb);
    lv_anim_set_values(&a, s_zoom[g], to);
    lv_anim_set_time(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* Select group g (-1 = none): it grows, the previous one shrinks back */
static void value_select(int g)
{
    if (g == s_sel) return;
    if (s_sel >= 0) group_animate(s_sel, 256);
    s_sel = g;
    if (g >= 0) group_animate(g, ZOOM_BIG);
}

/* A tap on the main screen: select the value under the finger, or none */
static void value_tap(lv_point_t p)
{
    for (int g = 0; g < GROUPS; g++) {
        if (group_page(g) != s_page) continue;
        lv_area_t a;
        group_area(g, &a);
        if (p.x >= a.x1 - 8 && p.x <= a.x2 + 8 && p.y >= a.y1 - 8 && p.y <= a.y2 + 8) {
            value_select(g == s_sel ? -1 : g);   /* tapping the active one again deselects it */
            return;
        }
    }
    value_select(-1);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);   /* swipes may start on a button */
    lv_obj_set_size(btn, 148, 46);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_center(label);
    return btn; /* no event callback: the buttons do nothing */
}

/* ---- Config panel: slides in from the right edge ---------------------------------
 * Swipe down from the top edge to open, swipe it back up to close. */
#define CFG_W        SCR_W
#define CFG_H        SCR_H
#define CFG_EDGE     24   /* a drag must start this close to the top edge to open */

static lv_obj_t *s_cfg = NULL;
static lv_obj_t *s_cfg_title = NULL;
static lv_obj_t *s_bri_caption = NULL;
#define CFG_PAGES    2    /* settings, errors */
#define NO_ERRORS    "No errors"
static lv_obj_t *s_err_box = NULL, *s_err_label = NULL;
static lv_obj_t *s_cfg_strip = NULL, *s_cfg_dot[CFG_PAGES];
static int s_cfg_page = 0;
enum { CFG_UNDECIDED, CFG_VERTICAL, CFG_PAGING, CFG_SCROLL };
static int s_cfg_mode;
static lv_coord_t s_err_scroll, s_err_scroll_start;   /* error text scrolled up by this much */

/* How far the error text can scroll: its height beyond the visible area */
static lv_coord_t err_scroll_max(void)
{
    lv_obj_update_layout(s_err_label);
    lv_coord_t m = lv_obj_get_height(s_err_label) - lv_obj_get_height(s_err_box);
    return m > 0 ? m : 0;
}

static void err_scroll_to(lv_coord_t y)
{
    lv_coord_t m = err_scroll_max();
    s_err_scroll = y < 0 ? 0 : y > m ? m : y;
    lv_obj_set_y(s_err_label, -s_err_scroll);
}                /* what a drag on the open panel does */
static void apply_panel_theme(bool light);
static void ha_errors(const char *errors);
static bool s_cfg_open = false;
static bool s_dragging = false;       /* dragging the config panel */
static bool s_paging = false;         /* dragging the sensor pages */
static lv_coord_t s_drag_start_x, s_drag_start_y;

static void cfg_anim_y(void *obj, int32_t y) { lv_obj_set_y(obj, y); }

static void cfg_strip_x(void *obj, int32_t x) { lv_obj_set_x(obj, x); }

static void cfg_show_page(int page)
{
    static const char *titles[CFG_PAGES] = {"Settings", "Errors"};
    if (page < 0) page = 0;
    if (page >= CFG_PAGES) page = CFG_PAGES - 1;
    s_cfg_page = page;
    lv_label_set_text(s_cfg_title, titles[page]);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_cfg_strip);
    lv_anim_set_exec_cb(&a, cfg_strip_x);
    lv_anim_set_values(&a, lv_obj_get_x(s_cfg_strip), -page * CFG_W);
    lv_anim_set_time(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    apply_panel_theme(s_light);
}

static void cfg_slide_to(bool open)
{
    s_cfg_open = open;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_cfg);
    lv_anim_set_exec_cb(&a, cfg_anim_y);
    lv_anim_set_values(&a, lv_obj_get_y(s_cfg), open ? 0 : -CFG_H);
    lv_anim_set_time(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* Registered on the main screen (to open) and on the panel background (to close) */
static void edge_swipe_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);

    if (code == LV_EVENT_PRESSED) {
        s_dragging = s_cfg_open || p.y <= CFG_EDGE;
        s_paging = !s_dragging;
        s_drag_start_x = p.x;
        s_drag_start_y = p.y;
        s_cfg_mode = s_cfg_open ? CFG_UNDECIDED : CFG_VERTICAL;
        s_swiped = false;
        if (s_dragging) lv_anim_del(s_cfg, cfg_anim_y);
        if (s_paging) lv_anim_del(s_strip, strip_anim_x);
    } else if (code == LV_EVENT_PRESSING && s_paging) {
        /* pages follow the finger; past the first/last page they resist (move at 1/4
         * speed, at most 30 px) and spring back on release, to show it's the end */
        lv_coord_t dx = p.x - s_drag_start_x;
        bool at_end = (dx > 0 && s_page == 0) || (dx < 0 && s_page == PAGE_COUNT - 1);
        if (at_end) {
            dx /= 4;
            if (dx > 30) dx = 30;
            if (dx < -30) dx = -30;
        } else {
            if (dx > PAGE_W) dx = PAGE_W;
            if (dx < -PAGE_W) dx = -PAGE_W;
        }
        lv_obj_set_x(s_strip, -s_page * PAGE_W + dx);
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && s_paging) {
        s_paging = false;
        lv_coord_t dx = p.x - s_drag_start_x;
        lv_coord_t dy = p.y - s_drag_start_y;
        if (code == LV_EVENT_RELEASED && LV_ABS(dx) < 8 && LV_ABS(dy) < 8) { value_tap(p); return; }
        show_page(dx <= -SWIPE_MIN ? s_page + 1 : dx >= SWIPE_MIN ? s_page - 1 : s_page);
    } else if (code == LV_EVENT_PRESSING && s_dragging) {
        lv_coord_t dx = p.x - s_drag_start_x;
        lv_coord_t dy = p.y - s_drag_start_y;
        if (s_cfg_mode == CFG_UNDECIDED) { /* decide by the first 8 px of movement */
            if (LV_ABS(dx) < 8 && LV_ABS(dy) < 8) return;
            s_swiped = true;
            if (LV_ABS(dx) > LV_ABS(dy)) {
                s_cfg_mode = CFG_PAGING;
                lv_anim_del(s_cfg_strip, cfg_strip_x);
            } else {
                /* errors page: scroll the text while it can move that way; dragging up
                 * with the text already at its end (or fitting) closes the panel */
                bool can = s_cfg_page == 1 && (dy > 0 ? s_err_scroll > 0 : s_err_scroll < err_scroll_max());
                s_cfg_mode = can ? CFG_SCROLL : CFG_VERTICAL;
                s_err_scroll_start = s_err_scroll;
            }
        }
        if (s_cfg_mode == CFG_PAGING) {
            bool at_end = (dx > 0 && s_cfg_page == 0) || (dx < 0 && s_cfg_page == CFG_PAGES - 1);
            if (at_end) { dx /= 4; if (dx > 30) dx = 30; if (dx < -30) dx = -30; }
            lv_obj_set_x(s_cfg_strip, -s_cfg_page * CFG_W + dx);
            return;
        }
        if (s_cfg_mode == CFG_SCROLL) { err_scroll_to(s_err_scroll_start - dy); return; }
        if (s_cfg_mode != CFG_VERTICAL) return;
        lv_coord_t y = (s_cfg_open ? 0 : -CFG_H) + dy;
        if (y > 0) y = 0;
        if (y < -CFG_H) y = -CFG_H;
        lv_obj_set_y(s_cfg, y);
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && s_dragging) {
        s_dragging = false;
        if (s_cfg_mode == CFG_PAGING) {
            lv_coord_t dx = p.x - s_drag_start_x;
            cfg_show_page(dx <= -SWIPE_MIN ? s_cfg_page + 1 : dx >= SWIPE_MIN ? s_cfg_page - 1 : s_cfg_page);
        } else if (s_cfg_mode == CFG_VERTICAL) {
            cfg_slide_to(lv_obj_get_y(s_cfg) > -CFG_H / 2);
        }
    }
}

static void create_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_anim_del_all();
    lv_obj_clean(scr);
    s_page = 0;
    s_sel = -1;
    memset(s_zoom, 0, sizeof(s_zoom));
    s_cfg_page = 0;
    s_cfg_open = false;
    s_dragging = s_paging = false;
    memset(s_val, 0, sizeof(s_val));
    memset(s_unit, 0, sizeof(s_unit));
    memset(s_name, 0, sizeof(s_name));
    memset(s_g_val_, 0, sizeof(s_g_val_));
    memset(s_g_unit_, 0, sizeof(s_g_unit_));
    memset(s_g_name_, 0, sizeof(s_g_name_));
    memset(s_chart_, 0, sizeof(s_chart_));
    s_err_box = s_err_label = NULL;
    memset(s_cfg_dot, 0, sizeof(s_cfg_dot));
    lv_obj_set_style_bg_color(scr, s_light ? lv_color_white() : lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, edge_swipe_cb, LV_EVENT_ALL, NULL);

    /* Main screen: sensor pages and status line */
    create_pages(scr);
    create_status_icons(scr, 0);

    /* Config panel, parked just above the top edge */
    s_cfg = lv_obj_create(scr);
    lv_obj_set_size(s_cfg, CFG_W, SCR_H);
    lv_obj_set_pos(s_cfg, 0, -CFG_H);
    lv_obj_set_style_radius(s_cfg, 0, 0);
    lv_obj_set_style_border_width(s_cfg, 0, 0);
    lv_obj_set_style_pad_all(s_cfg, 0, 0);
    lv_obj_clear_flag(s_cfg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_cfg, edge_swipe_cb, LV_EVENT_ALL, NULL);

    /* Two panel pages side by side (settings, errors), dragged left/right like the main screen */
    s_cfg_strip = lv_obj_create(s_cfg);
    lv_obj_remove_style_all(s_cfg_strip);
    lv_obj_set_size(s_cfg_strip, CFG_W * CFG_PAGES, SCR_H);
    lv_obj_clear_flag(s_cfg_strip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cfg_strip, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *cp[CFG_PAGES];
    for (int i = 0; i < CFG_PAGES; i++) {
        cp[i] = lv_obj_create(s_cfg_strip);
        lv_obj_remove_style_all(cp[i]);
        lv_obj_set_size(cp[i], CFG_W, SCR_H);
        lv_obj_set_pos(cp[i], i * CFG_W, 0);
        lv_obj_clear_flag(cp[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cp[i], LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    s_cfg_title = lv_label_create(s_cfg);
    lv_label_set_text(s_cfg_title, "Settings");
    lv_obj_set_style_text_font(s_cfg_title, &lv_font_montserrat_20, 0);
    lv_obj_align(s_cfg_title, LV_ALIGN_TOP_LEFT, 10, 19 - lv_font_montserrat_20.line_height / 2);

    lv_obj_t *btn = make_button(cp[0], "Theme", lv_color_hex(0x5E5E6E));
    /* portrait: one column; landscape: buttons left, brightness and orientation right */
    const lv_coord_t colx = s_landscape ? SCR_W / 4 : 0;   /* column offset from centre */
    if (s_landscape) lv_obj_align(btn, LV_ALIGN_TOP_MID, -colx, 96);
    else lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 106);
    lv_obj_add_event_cb(btn, ha_toggle_cb, LV_EVENT_CLICKED, NULL);
    s_ha_btn = btn;

    s_bri_caption = lv_label_create(cp[0]);
    lv_label_set_text(s_bri_caption, "Brightness");
    lv_obj_set_style_text_font(s_bri_caption, &lv_font_montserrat_14, 0);
    if (s_landscape) lv_obj_set_pos(s_bri_caption, SCR_W / 2 + (SCR_W / 2 - 148) / 2, 40);
    else lv_obj_align_to(s_bri_caption, btn, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 36);

    lv_obj_t *sl = lv_slider_create(cp[0]);
    lv_obj_set_width(sl, 132);
    lv_obj_align_to(sl, s_bri_caption, LV_ALIGN_OUT_BOTTOM_LEFT, 8, 16);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x18BCF2), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x18BCF2), LV_PART_KNOB);
    lv_obj_set_ext_click_area(sl, 12);
    lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_RELEASED, NULL);
    s_slider = sl;

    /* Orientation: the label names the action, like the theme button */
    lv_obj_t *ob = make_button(cp[0], s_landscape ? "Portrait" : "Landscape", lv_color_hex(0x5E5E6E));
    if (s_landscape) lv_obj_align(ob, LV_ALIGN_TOP_MID, colx, 96);
    else lv_obj_align(ob, LV_ALIGN_TOP_MID, 0, 246);
    lv_obj_add_event_cb(ob, orient_cb, LV_EVENT_CLICKED, NULL);
    s_orient_btn = ob;

    /* Errors page: HA errors from the last poll, scrolls when the text is long */
    lv_coord_t ey = STATUS_H + 6;
    /* A plain, non-clickable clip area: touches pass through to the panel, which scrolls
     * the text itself (see edge_swipe_cb) */
    s_err_box = lv_obj_create(cp[1]);
    lv_obj_remove_style_all(s_err_box);
    lv_obj_set_size(s_err_box, CFG_W - 20, SCR_H - ey - DOTS_H - 4);
    lv_obj_set_pos(s_err_box, 10, ey);
    lv_obj_clear_flag(s_err_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_err_label = lv_label_create(s_err_box);
    lv_label_set_long_mode(s_err_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_err_label, lv_pct(100));
    lv_obj_set_style_text_font(s_err_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_err_label, NO_ERRORS);

    /* Page dots at the bottom of the panel */
    for (int i = 0; i < CFG_PAGES; i++) {
        s_cfg_dot[i] = lv_obj_create(s_cfg);
        lv_obj_remove_style_all(s_cfg_dot[i]);
        lv_obj_set_size(s_cfg_dot[i], 8, 8);
        lv_obj_set_style_radius(s_cfg_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_cfg_dot[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_cfg_dot[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(s_cfg_dot[i], LV_ALIGN_BOTTOM_MID, (i * 2 - (CFG_PAGES - 1)) * 8, -(DOTS_H - 8) / 2);
    }

    /* Status icons on the panel too, since the panel covers the main screen's */
    create_status_icons(s_cfg, 1);
    show_status(HA_STATE_OFFLINE);
    apply_panel_theme(s_light);

    /* Refresh: re-read everything from Home Assistant now */
    s_refresh_btn = make_button(cp[0], "Connecting...", lv_color_hex(0x5E5E6E));
    lv_obj_align(s_refresh_btn, LV_ALIGN_TOP_MID, -colx, s_landscape ? 40 : 44);
    lv_obj_add_event_cb(s_refresh_btn, refresh_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_opa(s_refresh_btn, LV_OPA_50, LV_STATE_DISABLED);

    lv_obj_set_style_opa(btn, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_opa(sl, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_opa(s_orient_btn, LV_OPA_50, LV_STATE_DISABLED);
}

/* Panel colours for the current theme. Must be called with the LVGL lock held. */
static void apply_panel_theme(bool light)
{
    if (!s_cfg) return;
    lv_obj_set_style_bg_color(s_cfg, lv_color_hex(light ? 0xF4F6F9 : 0x15181D), 0);
    lv_color_t text = light ? lv_color_hex(0x1C1C1E) : lv_color_white();
    lv_obj_set_style_text_color(s_cfg_title, text, 0);
    lv_obj_set_style_text_color(s_bri_caption, text, 0);
    if (s_err_box) {
        bool none = strcmp(lv_label_get_text(s_err_label), NO_ERRORS) == 0;
        lv_obj_set_style_text_color(s_err_label, none ? lv_color_hex(light ? 0x5F6368 : 0x8A8F98)
                                                      : lv_color_hex(light ? 0xC62828 : 0xE74C3C), 0);
    }
    for (int i = 0; i < CFG_PAGES; i++) {
        if (!s_cfg_dot[i]) continue;
        lv_obj_set_style_bg_color(s_cfg_dot[i], i == s_cfg_page ? text : lv_color_hex(light ? 0xAEB4BE : 0x4A5058), 0);
    }
}

/* While a change is on its way to Home Assistant, the button and slider take no input.
 * Must be called with the LVGL lock held. */
static void set_busy(bool busy)
{
    s_busy = busy;
    lv_obj_t *objs[] = {s_ha_btn, s_slider, s_refresh_btn, s_orient_btn};
    for (int i = 0; i < 4; i++) {
        if (!objs[i]) continue;
        if (busy) lv_obj_add_state(objs[i], LV_STATE_DISABLED);
        else lv_obj_clear_state(objs[i], LV_STATE_DISABLED);
    }
}

/* Wi-Fi icon: green = connected, grey = not. HA dot: green = HA answered, red = not.
 * Must be called with the LVGL lock held. */

static void show_status(ha_state_t state)
{
    /* darker shades on the light background for contrast */
    const uint32_t ok  = s_light ? 0x1B7F3B : 0x2ECC71;
    const uint32_t bad = s_light ? 0xC62828 : 0xE74C3C;
    const uint32_t off = s_light ? 0x5F6368 : 0x8A8F98;
    bool wifi = state != HA_STATE_OFFLINE;
    uint32_t ha = (wifi && state != HA_STATE_ERROR) ? ok : bad;
    for (int i = 0; i < 2; i++) {
        if (!s_wifi_icon[i]) continue;
        lv_obj_set_style_text_color(s_wifi_icon[i], lv_color_hex(wifi ? ok : off), 0);
        lv_obj_set_style_bg_color(s_ha_icon[i], lv_color_hex(ha), 0);
    }
}

/* Network task: the request finished and the state was re-read */
static void ha_update_done(bool ok)
{
    if (!lvgl_port_lock(0)) return;
    set_busy(false);
    if (s_refreshing) { /* errors show in the panel's error box, not on the buttons */
        s_refreshing = false;
        lv_label_set_text(lv_obj_get_child(s_refresh_btn, 0), LV_SYMBOL_REFRESH " Refresh");
    }
    (void)ok;
    lvgl_port_unlock();
}

/* Network task: errors from the last poll */
static void ha_errors(const char *errors)
{
    if (!s_err_label || !lvgl_port_lock(0)) return;
    const char *t = errors[0] ? errors : NO_ERRORS;
    if (strcmp(lv_label_get_text(s_err_label), t) != 0) {
        lv_label_set_text(s_err_label, t);
        err_scroll_to(s_err_scroll);  /* keep the offset within the new text */
        apply_panel_theme(s_light);
    }
    lvgl_port_unlock();
}

static lv_color_t neutral_btn_color(void)
{
    return lv_color_hex(s_light ? 0x2C3440 : 0x5E5E6E);
}

/* Refresh button: re-read all entities right away */
static void refresh_cb(lv_event_t *e)
{
    if (s_busy || s_swiped) return;
    s_refreshing = true;
    set_busy(true);
    lv_obj_set_style_bg_color(s_refresh_btn, neutral_btn_color(), 0);
    lv_label_set_text(lv_obj_get_child(s_refresh_btn, 0), "Refreshing...");
    ha_client_refresh();
}

/* The button toggles the Home Assistant entity; the theme follows its state */
static void ha_toggle_cb(lv_event_t *e)
{
    if (s_busy || s_swiped) return;
    set_busy(true);
    lv_label_set_text(lv_obj_get_child(s_ha_btn, 0), "Updating...");
    ha_client_toggle();
}

/* Entity on = dark theme, off = light theme. Offline/error keeps the current theme. */
static void ha_state_changed(ha_state_t state)
{
    const char *text;

    switch (state) {
    case HA_STATE_ON:  s_light = false; text = "Light mode"; break;
    case HA_STATE_OFF: s_light = true;  text = "Dark mode";  break;
    case HA_STATE_ERROR: text = NULL; break; /* keep the label; errors go to the error box */
    case HA_STATE_UNKNOWN: text = "Unknown state"; break;
    default: text = NULL; break; /* not connected yet: the refresh button says so */
    }

    if (!s_ha_btn || !lvgl_port_lock(0)) return;
    lv_obj_t *label = lv_obj_get_child(s_ha_btn, 0);
    bool light = s_light;
    /* Light: white background, dark button, darker slider. Dark: black background. */
    lv_obj_set_style_bg_color(lv_scr_act(), light ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_bg_color(s_ha_btn,
        light ? lv_color_hex(0x2C3440) : lv_color_hex(0x5E5E6E), 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    apply_panel_theme(light);
    apply_temp_theme(light);
    if (s_orient_btn) lv_obj_set_style_bg_color(s_orient_btn, neutral_btn_color(), 0);
    if (s_refresh_btn && !s_refreshing) { /* follow theme */
        lv_obj_set_style_bg_color(s_refresh_btn, neutral_btn_color(), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_refresh_btn, 0), lv_color_white(), 0);
        lv_label_set_text(lv_obj_get_child(s_refresh_btn, 0),
                          state == HA_STATE_OFFLINE ? "Connecting..." : LV_SYMBOL_REFRESH " Refresh");
    }
    if (s_slider) {
        lv_color_t accent = lv_color_hex(light ? 0x0B7FAB : 0x18BCF2);
        lv_obj_set_style_bg_color(s_slider, lv_color_hex(light ? 0xAEB4BE : 0x3A3F47), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_slider, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_slider, accent, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_slider, accent, LV_PART_KNOB);
    }
    if (text) lv_label_set_text(label, text);
    show_status(state);
    lvgl_port_unlock();
}

/* Backlight follows input_number.dev_brightness, mapped from its min..max to
 * 5-100 % (5 % floor so the screen never goes fully dark). */
static float s_bri_min = 0, s_bri_max = 100;

static void apply_backlight(float value)
{
    float span = s_bri_max - s_bri_min;
    int pct = span > 0 ? (int)((value - s_bri_min) * 100.0f / span + 0.5f) : 100;
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    bsp_display_set_brightness((uint8_t)pct);
}

static void ha_brightness_changed(float value, float min, float max)
{
    ESP_LOGI(TAG, "brightness %.1f (range %.1f-%.1f)", (double)value, (double)min, (double)max);
    s_bri_min = min;
    s_bri_max = max;
    apply_backlight(value);

    if (!s_slider || !lvgl_port_lock(0)) return;
    if (!lv_obj_has_state(s_slider, LV_STATE_PRESSED)) { /* don't fight a drag in progress */
        lv_slider_set_range(s_slider, (int32_t)min, (int32_t)max);
        lv_slider_set_value(s_slider, (int32_t)(value + 0.5f), LV_ANIM_OFF);
    }
    lvgl_port_unlock();
}

/* Dragging previews the brightness; releasing sends it to Home Assistant */
static void slider_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    float v = (float)lv_slider_get_value(sl);
    if (s_busy) return;
    apply_backlight(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        set_busy(true);
        ha_client_set_brightness(v);
    }
}

static esp_err_t app_lvgl_init(void)
{
    /* Initialize LVGL */
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,       /* LVGL task priority */
        .task_stack = 1024 * 10,  /* LVGL task stack size */
        .task_affinity = -1,      /* LVGL task pinned to core (-1 is no affinity) */
        .task_max_sleep_ms = 500, /* Maximum sleep in LVGL task */
        .timer_period_ms = 5      /* LVGL timer tick period in ms */
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port initialization failed");

    /* Add LCD screen */
    ESP_LOGD(TAG, "Add LCD screen");
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_DRAW_BUFF_HEIGHT,
        .double_buffer = EXAMPLE_LCD_DRAW_BUFF_DOUBLE,
        .hres = EXAMPLE_LCD_H_RES,
        .vres = EXAMPLE_LCD_V_RES,
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = true,
#endif
        }};
#if EXAMPLE_DISPLAY_ROTATION == 90
    disp_cfg.rotation.swap_xy = true;
    disp_cfg.rotation.mirror_x = true;
    disp_cfg.rotation.mirror_y = false;
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 34));
#elif EXAMPLE_DISPLAY_ROTATION == 180
    disp_cfg.rotation.swap_xy = false;
    disp_cfg.rotation.mirror_x = true;
    disp_cfg.rotation.mirror_y = true;
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 34, 0));
#elif EXAMPLE_DISPLAY_ROTATION == 270
    disp_cfg.rotation.swap_xy = true;
    disp_cfg.rotation.mirror_x = false;
    disp_cfg.rotation.mirror_y = true;
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 34));
#else
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 34, 0));
#endif
    lvgl_disp = lvgl_port_add_disp(&disp_cfg);

    /* Add touch input (for selected screen) */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lvgl_disp,
        .handle = touch_handle,
    };
    lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);

    return ESP_OK;
}


/* ---- BOOT button (GPIO9, low when pressed): next page on the visible screen, wrapping
 * around, on release. Polled from an LVGL timer, so the UI is touched in the LVGL task. */
#define BUTTON_GPIO GPIO_NUM_9

static void button_poll(lv_timer_t *t)
{
    static bool was_down = false;
    bool down = gpio_get_level(BUTTON_GPIO) == 0;
    if (!down && was_down) {   /* act on release, not on press */
        if (s_cfg_open) cfg_show_page((s_cfg_page + 1) % CFG_PAGES);
        else show_page((s_page + 1) % PAGE_COUNT);
    }
    was_down = down;
}

static void button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    lv_timer_create(button_poll, 30, NULL);  /* 30 ms polling also debounces */
}

/* ---- Orientation: portrait (172x320) or landscape (320x172), kept in NVS ---------- */
static void apply_rotation(void)
{
    /* the panel's 172 px side sits 34 px into the controller's 240 px RAM */
    if (s_landscape) esp_lcd_panel_set_gap(panel_handle, 0, 34);
    else esp_lcd_panel_set_gap(panel_handle, 34, 0);
    lv_disp_set_rotation(lvgl_disp, s_landscape ? LV_DISP_ROT_90 : LV_DISP_ROT_NONE);
}

/* Rebuild the UI in the orientation HA asked for. Runs in the LVGL task via lv_async_call
 * (also because the button that started a toggle is deleted by the rebuild). */
static void orient_apply(void *arg)
{
    bool want = (bool)(intptr_t)arg;
    if (want == s_landscape) return;
    bool was_open = s_cfg_open;
    s_landscape = want;
    nvs_handle_t nv;                        /* boot in the last orientation, before HA answers */
    if (nvs_open("ui", NVS_READWRITE, &nv) == ESP_OK) {
        nvs_set_u8(nv, "landscape", s_landscape);
        nvs_commit(nv);
        nvs_close(nv);
    }
    apply_rotation();
    create_ui();
    if (was_open) { lv_obj_set_y(s_cfg, 0); s_cfg_open = true; }
    lv_obj_invalidate(lv_scr_act());
    /* refill values, theme and graph from HA; the UI unlocks when that finishes */
    s_refreshing = true;
    set_busy(true);
    lv_label_set_text(lv_obj_get_child(s_refresh_btn, 0), "Refreshing...");
    ha_client_refresh();
}

/* Network task: orientation entity state */
static void ha_landscape_changed(bool landscape)
{
    if (!lvgl_port_lock(0)) return;
    if (landscape != s_landscape) lv_async_call(orient_apply, (void *)(intptr_t)landscape);
    lvgl_port_unlock();
}

/* Orientation button: toggle the HA entity; the layout follows when HA reports it */
static void orient_cb(lv_event_t *e)
{
    if (s_busy || s_swiped) return;
    set_busy(true);
    ha_client_toggle_landscape();
}
