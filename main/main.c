#include "screenshot.h"
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

/* Montserrat with umlauts etc. (tools/make_fonts.py); symbols fall back to the built-ins */
LV_FONT_DECLARE(lv_font_latin_12)
LV_FONT_DECLARE(lv_font_latin_14)
LV_FONT_DECLARE(lv_font_latin_16)
LV_FONT_DECLARE(lv_font_latin_22)
LV_FONT_DECLARE(lv_font_clock_24)
LV_FONT_DECLARE(lv_font_clock_28)
LV_FONT_DECLARE(lv_font_clock_32)
LV_FONT_DECLARE(lv_font_clock_36)
LV_FONT_DECLARE(lv_font_clock_40)
LV_FONT_DECLARE(lv_font_clock_44)
LV_FONT_DECLARE(lv_font_clock_48)
LV_FONT_DECLARE(lv_font_clock_56)
LV_FONT_DECLARE(lv_font_clock_64)
LV_FONT_DECLARE(lv_font_clock_72)


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
static void ha_busy(bool busy);
static void ha_button_changed(int page, int row, ha_btn_state_t state, const char *name);
static void ha_button_pressed(int page, int row, bool ok);
static void ha_picture_changed(int page, int row, uint16_t *px, int w, int h, int zoom, const char *name);
static void ha_forecast_changed(int page, int row, const ha_forecast_t *f, int n, bool hourly);
static void ha_weather_changed(int page, int row, const char *cond, const char *temp, const char *unit,
                               const char *hum, const char *name);
static void apply_panel_theme(bool light);
static void ha_sensor_changed(int page, int row, const char *value, const char *unit, const char *name);
static void ha_layout_changed(const ha_layout_t *layout);
static void ha_icon_changed(int page, int row, const char *icon);
static void dots_show(void);
static void show_page(int page);
static void test_tap(int x, int y);
static void value_tap(lv_point_t p);

static void ha_graph_changed(int graph, int row, const char *latest, const char *unit, const char *name,
                             const float *pts, int n, int64_t end, int hours, bool zero_based);
static void place_graph_value(void);
static float nice_ceil(float x);
static void ha_state_changed(ha_state_t state);
static void ha_brightness_changed(float value, float min, float max);
static void slider_cb(lv_event_t *e);
static void ha_update_done(bool ok, bool refresh);
static void ha_errors(const char *errors);
static bool s_busy = false;
static bool s_pres_on = false;        /* presentation mode (see "Presentation mode") */
static uint16_t s_pres_secs = 10;   /* a refresh or view change is on its way */
/* set locally, not yet confirmed by HA: HA's (older) value from a poll is ignored */
static bool s_theme_pending, s_orient_pending, s_bri_pending, s_pres_pending;
static bool s_theme_known;     /* the theme entity's state is known (label shows the action) */
static bool s_offline_state = true;
static void apply_theme(void);
static void ui_refill(void);
static bool s_swiped = false;   /* this touch became a swipe: buttons ignore its release */
static bool s_light = false;   /* current theme */
static int s_boot_page = -1;   /* page saved before the last reboot, until the pages exist */
static bool s_refreshing = false;
static lv_obj_t *s_refresh_btn = NULL;
static void refresh_cb(lv_event_t *e);
static void view_cb(lv_event_t *e);
static void view_btn_update(void);
static void view_menu_close(void);
static void view_select(int i);
static void cfg_test_open(int page);
/* Dashboard views for the view selector (LVGL lock) */
static layout_view_t s_views[LAYOUT_MAX_VIEWS];
static int s_nviews, s_cur_view = -1;
static char s_view_path[32];          /* chosen view, kept in NVS; "" = the first */
static lv_obj_t *s_view_btn = NULL, *s_view_caption = NULL;
static lv_obj_t *s_view_menu = NULL;  /* open view list (its backdrop), NULL when closed */
static bool s_view_pending = false;   /* a view change is on its way */
static void ha_views_changed(const layout_view_t *views, int n, int current);
static void ha_dashes_changed(const layout_view_t *dashes, int n, int current);
static layout_view_t s_dashes[LAYOUT_MAX_VIEWS];
static int s_ndash, s_cur_dash = -1;
static char s_dash_path[32];          /* chosen dashboard, kept in NVS; "" = HA_DASHBOARD */
static int s_ha_page = -1;            /* page as HA_PAGE_ENTITY last had it (0-based), -1 unknown */
static lv_obj_t *s_dash_btn = NULL, *s_dash_caption = NULL;
static void dash_cb(lv_event_t *e);
static void dash_btn_update(void);
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
    lv_obj_set_style_text_font(wifi, &lv_font_latin_14, 0);
    lv_obj_align_to(wifi, dot, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    s_ha_icon[set] = dot;
    s_wifi_icon[set] = wifi;
}
static lv_obj_t *s_slider = NULL;
static lv_obj_t *s_orient_btn = NULL;
static bool s_landscape = false;   /* orientation, kept in NVS */
static void orient_cb(lv_event_t *e);
static void apply_rotation(void);

extern const lv_img_dsc_t bg_dark;
extern const lv_img_dsc_t bg_light;

/* Theme background image (centred 320x320, covers both orientations). */
static void set_screen_bg(lv_obj_t *scr, bool light)
{
    lv_obj_set_style_bg_color(scr, light ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_bg_img_src(scr, light ? &bg_light : &bg_dark, 0);
}
static void ha_landscape_changed(bool landscape);
static void ha_pres_changed(bool on, int secs);
static void ha_nav_changed(const char *dash, const char *view, int page);
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
        uint8_t page = 0;
        if (nvs_open("ui", NVS_READONLY, &nv) == ESP_OK) {
            nvs_get_u8(nv, "landscape", &land);
            if (nvs_get_u8(nv, "page", &page) == ESP_OK) s_boot_page = page;
            size_t vlen = sizeof(s_view_path);
            if (nvs_get_str(nv, "view", s_view_path, &vlen) != ESP_OK) s_view_path[0] = 0;
            uint8_t pon = 0;
            if (nvs_get_u8(nv, "pres_on", &pon) == ESP_OK) s_pres_on = pon;
            nvs_get_u16(nv, "pres_s", &s_pres_secs);
            if (s_pres_secs < 1) s_pres_secs = 10;
            size_t dlen = sizeof(s_dash_path);
            if (nvs_get_str(nv, "dash", s_dash_path, &dlen) != ESP_OK) s_dash_path[0] = 0;
            nvs_close(nv);
        }
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
    ha_client_set_pres_cb(ha_pres_changed);
    ha_client_set_layout_cb(ha_layout_changed);
    ha_client_set_busy_cb(ha_busy);
    ha_client_set_button_cb(ha_button_changed);
    ha_client_set_press_cb(ha_button_pressed);
    ha_client_set_picture_cb(ha_picture_changed);
    ha_client_set_weather_cb(ha_weather_changed);
    ha_client_set_forecast_cb(ha_forecast_changed);
    ha_client_set_icon_cb(ha_icon_changed);
    ha_client_set_views_cb(ha_views_changed);
    ha_client_set_dashboards_cb(ha_dashes_changed);
    ha_client_set_nav_cb(ha_nav_changed);
    ha_client_set_dashboard(s_dash_path, s_view_path);
    screenshot_start();
    screenshot_set_touch_cb(dots_show);
    screenshot_set_page_cb(show_page);
    screenshot_set_tap_cb(test_tap);
    screenshot_set_view_cb(view_select);
    screenshot_set_cfg_cb(cfg_test_open);
    ha_client_start(ha_state_changed, ha_brightness_changed, ha_update_done, ha_sensor_changed, ha_graph_changed);
}

#include "config.h"
#include "nvs.h"
#include "names.h"
#include "tz.h"
#include "icons.h"
#include "esp_log.h"


/* Screen size in the current orientation (portrait 172x320, landscape 320x172) */
#define SCR_W  lv_disp_get_hor_res(NULL)
#define SCR_H  lv_disp_get_ver_res(NULL)
/* value / unit / name fonts: smaller in landscape, where three values share one row */
#define VFONT  (s_landscape ? &lv_font_montserrat_28 : &lv_font_montserrat_36)
#define UFONT  (s_landscape ? &lv_font_latin_16 : &lv_font_latin_22)
#define NFONT  (s_landscape ? &lv_font_latin_14 : &lv_font_latin_16)
#define NAME_DY (s_landscape ? 34 : 44)       /* name label below the value's top */

/* ---- Main screen: pages dragged left/right, with page dots at the bottom ----------
 * Sensor pages (temperature, humidity) first, then the graph page.
 * Rows share the height between the status line and the dots evenly. */
#define STATUS_H   30                    /* status line: page title, Wi-Fi icon, HA dot */
#define CARD_INSET 0                     /* gap between a card panel and its block: none, cards
                                          * meet the screen edges and their neighbours */
#define BTN_INSET  8                     /* buttons keep a gap inside their block */
#define TOP_M      0                     /* pages start this close to the top: the status line
                                          * floats over them and fades out like the page dots */
#define DOTS_H     22                    /* bottom strip for the page dots (config panel) */
#define BOTTOM_M   0                     /* page content reaches this close to the bottom: the
                                          * page dots float over it and fade out */
#define DOTS_SHOW_MS 3000                /* page dots fade out this long after the last touch */
#define PAGE_W     SCR_W
#define SWIPE_MIN  40                    /* drag distance that flips the page */
static ha_layout_t s_layout;                  /* pages from the HA dashboard (LVGL lock) */
#define PAGE_COUNT (s_layout.count)
#define KIND(pg, i)  (s_layout.pages[pg].items[i].kind)   /* each card is its own block */
#define ROWS(pg)     (s_layout.pages[pg].count)

/* Where row i's value goes: portrait = rows stacked, landscape = side-by-side columns.
 * cx = horizontal centre, y = top of the value. */
static void value_pos(int pg, int i, lv_coord_t *cx, lv_coord_t *y);
/* Top of a value label whose digits are centred on y = c (name and icon hang off it) */
static lv_coord_t value_top(lv_coord_t c)
{
    const lv_font_t *f = s_landscape ? &lv_font_montserrat_28 : &lv_font_montserrat_36;
    lv_font_glyph_dsc_t g;
    lv_coord_t digit_h = lv_font_get_glyph_dsc(f, &g, '0', 0) ? g.box_h + g.ofs_y : f->line_height / 2;
    lv_coord_t baseline = f->line_height - f->base_line;   /* from the label's top */
    return c - baseline + digit_h / 2;
}

static lv_coord_t row_y(int i, int rows)
{
    lv_coord_t slot = (SCR_H - TOP_M - BOTTOM_M) / LV_MAX(rows, 1);
    return value_top(TOP_M + i * slot + slot / 2);
}

static lv_obj_t *s_strip = NULL;                         /* all pages side by side */
static lv_obj_t *s_name[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static lv_obj_t *s_val[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static lv_obj_t *s_unit[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static lv_obj_t *s_dot[LAYOUT_MAX_PAGES];
static lv_obj_t *s_dots_box;                             /* pill holding the dots, fades */
static lv_timer_t *s_dots_timer;

static bool s_dots_on = true;     /* within DOTS_SHOW_MS of the last touch */
static bool s_status_quiet;       /* HA fine and nothing being read: the HA dot may fade */
static bool s_wifi_quiet;         /* Wi-Fi connected: its icon may fade */

static void dots_opa_cb(void *o, int32_t v) { lv_obj_set_style_opa(o, v, 0); }
static void fade_obj(lv_obj_t *o, lv_opa_t to, uint32_t ms)
{
    if (!o) return;
    lv_anim_del(o, dots_opa_cb);
    lv_opa_t from = lv_obj_get_style_opa(o, 0);
    if (from == to) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, dots_opa_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    lv_anim_start(&a);
}
static void dots_fade(lv_opa_t to, uint32_t ms) { fade_obj(s_dots_box, to, ms); }

/* Main-screen status icons: shown on touch and whenever something needs attention */
static lv_obj_t *s_status_bg;      /* status line background, shown on touch */
static lv_obj_t *s_page_title, *s_title_icon;
static void status_icons_update(void)
{
    /* page title, its icon and the bar behind the status line: only while touched */
    fade_obj(s_status_bg, s_dots_on ? LV_OPA_COVER : LV_OPA_TRANSP, s_dots_on ? 200 : 600);
    fade_obj(s_page_title, s_dots_on ? LV_OPA_COVER : LV_OPA_TRANSP, s_dots_on ? 200 : 600);
    fade_obj(s_title_icon, s_dots_on ? LV_OPA_COVER : LV_OPA_TRANSP, s_dots_on ? 200 : 600);
    bool ha = s_dots_on || !s_status_quiet, wifi = s_dots_on || !s_wifi_quiet;   /* refresh: HA dot only */
    fade_obj(s_wifi_icon[0], wifi ? LV_OPA_COVER : LV_OPA_TRANSP, wifi ? 200 : 600);
    fade_obj(s_ha_icon[0], ha ? LV_OPA_COVER : LV_OPA_TRANSP, ha ? 200 : 600);
}

static void dots_timer_cb(lv_timer_t *t)
{
    (void)t;
    lv_timer_pause(s_dots_timer);
    s_dots_on = false;
    dots_fade(LV_OPA_TRANSP, 600);
    status_icons_update();
}

/* Show the page dots now; they fade out DOTS_SHOW_MS later (restarted by every touch) */
static void dots_show(void)
{
    s_dots_on = true;
    status_icons_update();
    if (!s_dots_box) return;
    dots_fade(LV_OPA_COVER, 200);
    if (!s_dots_timer) s_dots_timer = lv_timer_create(dots_timer_cb, DOTS_SHOW_MS, NULL);
    lv_timer_reset(s_dots_timer);
    lv_timer_resume(s_dots_timer);
}
static int s_page = 0;
/* s_page_title, s_title_icon: page name and its icon, left side of the status line */

/* graph page */
/* graphs: one set per page and row; s_gi = GI(page, row) selects which one the s_g_* / s_chart
 * names mean */
#define GI(pg, row)  ((pg) * LAYOUT_MAX_ROWS + (row))
#define GPG(gi)      ((gi) / LAYOUT_MAX_ROWS)
#define GROW(gi)     ((gi) % LAYOUT_MAX_ROWS)
#define GRAPHS       (LAYOUT_MAX_PAGES * LAYOUT_MAX_ROWS)
static int s_gi = 0;
static lv_obj_t *s_g_name_[GRAPHS], *s_g_val_[GRAPHS], *s_g_unit_[GRAPHS], *s_chart_[GRAPHS];
static lv_chart_series_t *s_chart_ser_[GRAPHS];
#define s_g_name    s_g_name_[s_gi]
#define s_g_val     s_g_val_[s_gi]
#define s_g_unit    s_g_unit_[s_gi]
#define s_chart     s_chart_[s_gi]
#define s_chart_ser s_chart_ser_[s_gi]


/* ---- Card icons (values, graphs, buttons): Material Design Icons from HA ---- */
static lv_obj_t *s_icon[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static lv_img_dsc_t s_icon_dsc[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static uint8_t *s_icon_px[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];   /* resampled bitmap (not 24 px), owned */
static uint8_t s_icon_sz[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static char s_icon_name[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS][48];   /* kept across UI rebuilds */
#define ICON_GAP 6
#define GRAPH_ICON 32   /* px, the icon at a graph card's top right */
/* the card has room set aside for an icon (unless show_icon: false) */
static bool icon_slot(int pg, int i)
{
    uint8_t k = KIND(pg, i);
    return (k == LAYOUT_VALUES || k == LAYOUT_GRAPH || k == LAYOUT_BUTTONS || k == LAYOUT_GLANCE) &&
           s_layout.pages[pg].items[i].icon[0] != '-';
}
/* portrait tiles: icon on the left of the value, which moves right by this much */
/* portrait tiles: the icon sits left of the centred value; this much room is kept free on
 * both sides of it */
#define ICON_SHIFT(pg, i) (!s_landscape && KIND(pg, i) == LAYOUT_VALUES && icon_slot(pg, i) ? ICON_SIZE + ICON_GAP : 0)

static void icon_show(int pg, int i)
{
    lv_obj_t *o = s_icon[pg][i];
    if (!o) return;
    const uint8_t *bm = icon_bitmap(s_icon_name[pg][i]);
    if (!bm) { lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); return; }
    int n = s_icon_sz[pg][i] ? s_icon_sz[pg][i] : ICON_SIZE;
    if (n != ICON_SIZE) {                 /* LVGL can't zoom alpha-only images: resample */
        if (!s_icon_px[pg][i]) s_icon_px[pg][i] = malloc(n * n / 2);
        if (!s_icon_px[pg][i]) { lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); return; }
        icon_scale(bm, s_icon_px[pg][i], n);
        bm = s_icon_px[pg][i];
    }
    lv_img_dsc_t *d = &s_icon_dsc[pg][i];
    d->header.always_zero = 0;
    d->header.cf = LV_IMG_CF_ALPHA_4BIT;
    d->header.w = n;
    d->header.h = n;
    d->data_size = n * n / 2;
    d->data = bm;
    lv_img_cache_invalidate_src(d);
    lv_img_set_src(o, d);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void icons_free(void)
{
    for (int p = 0; p < LAYOUT_MAX_PAGES; p++)
        for (int i = 0; i < LAYOUT_MAX_ROWS; i++) { free(s_icon_px[p][i]); s_icon_px[p][i] = NULL; }
    memset(s_icon_sz, 0, sizeof(s_icon_sz));
}

static lv_obj_t *icon_create(lv_obj_t *parent, int pg, int i, lv_color_t color, int size)
{
    lv_obj_t *o = lv_img_create(parent);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, size, size);
    s_icon_sz[pg][i] = size;
    lv_obj_set_style_img_recolor(o, color, 0);
    lv_obj_set_style_img_recolor_opa(o, LV_OPA_COVER, 0);
    s_icon[pg][i] = o;
    icon_show(pg, i);
    return o;
}

/* Page title in the status line, with the heading's icon in front when it has one */
static lv_img_dsc_t s_title_icon_dsc;
#define TITLE_ICON 20   /* px (even: 4 bpp rows), a bit bigger than the Wi-Fi symbol */
static void title_update(void)
{
    if (!s_page_title) return;
    const layout_page_t *p = &s_layout.pages[s_page];
    lv_label_set_text(s_page_title, p->title);
    const uint8_t *bm = p->title[0] ? icon_bitmap(p->title_icon) : NULL;
    lv_coord_t x = 10;
    if (s_title_icon) {
        if (bm) {
            static uint8_t px[TITLE_ICON * TITLE_ICON / 2];
            icon_scale(bm, px, TITLE_ICON);   /* LVGL can't zoom alpha-only images */
            s_title_icon_dsc.header.cf = LV_IMG_CF_ALPHA_4BIT;
            s_title_icon_dsc.header.w = TITLE_ICON;
            s_title_icon_dsc.header.h = TITLE_ICON;
            s_title_icon_dsc.data_size = sizeof(px);
            s_title_icon_dsc.data = px;
            lv_img_cache_invalidate_src(&s_title_icon_dsc);
            lv_img_set_src(s_title_icon, &s_title_icon_dsc);
            lv_obj_clear_flag(s_title_icon, LV_OBJ_FLAG_HIDDEN);
            x = 8 + TITLE_ICON + 5;
        } else lv_obj_add_flag(s_title_icon, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_pos(s_page_title, x, 19 - lv_font_latin_16.line_height / 2);   /* centred on the status icons */
    lv_obj_set_width(s_page_title, SCR_W - x - 52);   /* short of the Wi-Fi icon and HA dot */
}

/* The two text colours of the cards: MAIN for values, SEC for everything around them
 * (names, titles, units, icons, ...): lighter than MAIN in light mode, darker in dark mode */
#define COL_MAIN(light) lv_color_hex((light) ? 0x2D4152 : 0xE1E4E7)   /* light: HA's tile text */
#define COL_SEC(light)  lv_color_hex((light) ? 0x3A4C5D : 0xB4BBC2)
static lv_color_t icon_color(void) { return COL_SEC(s_light); }

static void graph_block(int gi, lv_area_t *a);
static void value_pos(int pg, int i, lv_coord_t *cx, lv_coord_t *y);
#define GRAPH_X 10   /* left margin of a graph's name and value inside its block */
/* Tiles: left of the value (portrait) or above it (landscape columns); graphs: top right */
static void icon_place(int pg, int i)
{
    lv_obj_t *o = s_icon[pg][i];
    if (!o) return;
    if (KIND(pg, i) == LAYOUT_GRAPH) {
        lv_area_t b;
        graph_block(GI(pg, i), &b);
        lv_obj_set_pos(o, b.x2 + 1 - GRAPH_X + 2 - GRAPH_ICON, b.y1 + 2);
        return;
    }
    lv_coord_t cx, y;
    value_pos(pg, i, &cx, &y);
    const lv_font_t *vf = s_landscape ? &lv_font_montserrat_28 : &lv_font_montserrat_36;
    if (s_landscape) lv_obj_set_pos(o, cx - ICON_SIZE / 2, y - ICON_SIZE - 2);
    else {                                /* just left of the (centred) value */
        lv_coord_t x = 6;
        if (s_val[pg][i]) {
            lv_obj_update_layout(s_val[pg][i]);
            x = lv_obj_get_x(s_val[pg][i]) - ICON_GAP - ICON_SIZE;
        }
        /* a wide value takes the room: no icon rather than a cut value */
        if (x < 2) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else if (icon_bitmap(s_icon_name[pg][i])) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(o, LV_MAX(x, 2), y + (vf->line_height - ICON_SIZE) / 2);
    }
}

/* Centre the value horizontally in its row and hang the unit off its right side,
 * on the same baseline. Call after changing either text (LVGL lock held). */
static void value_pos(int pg, int i, lv_coord_t *cx, lv_coord_t *y)
{
    if (s_landscape) {
        lv_coord_t colw = SCR_W / LV_MAX(ROWS(pg), 1);
        *cx = i * colw + colw / 2;
        *y = value_top(TOP_M + (SCR_H - TOP_M - BOTTOM_M) / 2);
    } else {
        *cx = SCR_W / 2;
        *y = row_y(i, ROWS(pg));
    }
}

/* ---- Name labels: shortened when too wide ---- */
static char s_full[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS][32];   /* names before shortening */

static int name_width(const char *txt, void *ctx)
{
    (void)ctx;
    lv_point_t sz;
    lv_txt_get_size(&sz, txt, NFONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

/* Widest a name may be: its block (row/column) minus the margins */
static lv_coord_t name_max_w(int pg, int i)
{
    lv_area_t b;
    graph_block(GI(pg, i), &b);
    lv_coord_t w = lv_area_get_width(&b) - (KIND(pg, i) == LAYOUT_GRAPH ? 2 * GRAPH_X : 8);
    /* room for the icon: right of a graph's name, left of a portrait tile's centred value */
    if (KIND(pg, i) == LAYOUT_GRAPH && icon_slot(pg, i)) w -= GRAPH_ICON + ICON_GAP - 2;
    w -= 2 * ICON_SHIFT(pg, i);
    return w;
}

/* Text centred on cx below the value; ends in "..." if it still doesn't fit */
static void set_name_label(lv_obj_t *l, const char *txt, lv_coord_t max_w, lv_coord_t cx, lv_coord_t y, bool left, int lines)
{
    lv_label_set_text(l, txt);
    bool fits = name_width(txt, NULL) <= max_w;
    lv_label_set_long_mode(l, fits ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, fits ? LV_SIZE_CONTENT : max_w);
    /* up to `lines` lines: with a growing height LONG_DOT wraps on instead of ending in "..." */
    lv_obj_set_height(l, fits ? LV_SIZE_CONTENT : lv_obj_get_style_text_font(l, 0)->line_height * lines);
    lv_obj_set_style_text_align(l, left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
    if (left) lv_obj_align(l, LV_ALIGN_TOP_LEFT, cx, y + NAME_DY);
    else lv_obj_align(l, LV_ALIGN_TOP_MID, cx - SCR_W / 2, y + NAME_DY);
}

/* Re-apply all row names of value page pg, dropping words they all share if one is too wide */
static void graph_name_pos(lv_coord_t *cx, lv_coord_t *y);
static void page_names_update(int pg)
{
    int saved_gi = s_gi;
    /* tiles and graphs are shortened as separate groups (shared words within each kind) */
    static const uint8_t kinds[] = { LAYOUT_VALUES, LAYOUT_GRAPH };
    for (int k = 0; k < 2; k++) {
        char n[LAYOUT_MAX_ROWS][32];
        int idx[LAYOUT_MAX_ROWS], cnt = 0;
        lv_coord_t max_w = LV_COORD_MAX;
        for (int i = 0; i < ROWS(pg); i++) {
            if (KIND(pg, i) != kinds[k]) continue;
            strcpy(n[cnt], s_full[pg][i]);
            idx[cnt++] = i;
            max_w = LV_MIN(max_w, name_max_w(pg, i));
        }
        if (!cnt) continue;
        names_abbreviate(n, cnt, max_w, name_width, NULL);
        for (int c = 0; c < cnt; c++) {
            int i = idx[c];
            lv_coord_t cx, y;
            if (kinds[k] == LAYOUT_GRAPH) {   /* left-aligned at the top of the graph block */
                s_gi = GI(pg, i);
                if (!s_g_name) continue;
                graph_name_pos(&cx, &y);
                set_name_label(s_g_name, n[c], name_max_w(pg, i), cx, y - NAME_DY, true, 1);
            } else {
                if (!s_name[pg][i]) continue;
                value_pos(pg, i, &cx, &y);
                /* tiles: the full card width (the icon sits beside the value, not the name);
                 * portrait rows have room for a second line */
                lv_area_t b;
                graph_block(GI(pg, i), &b);
                set_name_label(s_name[pg][i], s_landscape ? n[c] : s_full[pg][i],
                               lv_area_get_width(&b) - 8, cx, y, false, s_landscape ? 1 : 2);
            }
        }
    }
    s_gi = saved_gi;
}

/* A value plus its unit wider than max_w (long text states) ends in "..." on one line */
static void fit_value(lv_obj_t *val, lv_obj_t *unit, lv_coord_t max_w)
{
    lv_point_t vs, us;
    lv_txt_get_size(&vs, lv_label_get_text(val), VFONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_txt_get_size(&us, lv_label_get_text(unit), UFONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_coord_t room = max_w - (us.x ? us.x + 3 : 0);
    bool fits = vs.x <= room;
    lv_label_set_long_mode(val, fits ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    lv_obj_set_width(val, fits ? LV_SIZE_CONTENT : LV_MAX(room, 20));
    lv_obj_set_height(val, fits ? LV_SIZE_CONTENT : ((const lv_font_t *)VFONT)->line_height);
}

/* Value centred on cx with the unit hanging off its right side, same baseline */
static void page_relayout(int pg);
/* left = false: value centred on x; left = true: value's left edge at x */
static void place_pair(lv_obj_t *val, lv_obj_t *unit, lv_coord_t x, lv_coord_t y, bool left)
{
    /* align on the untranslated positions; page_relayout re-applies the active shifts */
    lv_obj_set_style_translate_x(val, 0, 0);
    lv_obj_set_style_translate_y(val, 0, 0);
    lv_obj_set_style_translate_x(unit, 0, 0);
    lv_obj_set_style_translate_y(unit, 0, 0);
    if (left) lv_obj_align(val, LV_ALIGN_TOP_LEFT, x, y);
    else lv_obj_align(val, LV_ALIGN_TOP_MID, x - SCR_W / 2, y);
    lv_obj_update_layout(val);
    lv_obj_align_to(unit, val, LV_ALIGN_OUT_RIGHT_BOTTOM, 3,
                    -(((const lv_font_t *)VFONT)->base_line - ((const lv_font_t *)UFONT)->base_line));
}

static bool s_crisp[LAYOUT_MAX_PAGES * LAYOUT_MAX_ROWS];   /* see "Tapped value grows" */
static void crisp_off(int g);
static void crisp_on(int g);
static void place_value(int pg, int row)
{
    lv_coord_t cx, y;
    int g = GI(pg, row);
    bool crisp = s_crisp[g];
    crisp_off(g);
    value_pos(pg, row, &cx, &y);
    place_pair(s_val[pg][row], s_unit[pg][row], cx, y, false);
    icon_place(pg, row);                  /* follows the value's width */
    page_relayout(pg);
    if (crisp) crisp_on(g);
}

/* Sensor text and page dot colours for the current theme. LVGL lock must be held. */
/* ---- Button pages ---- */
static lv_obj_t *s_btn[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static lv_obj_t *s_btn_name[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static lv_obj_t *s_btn_state[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static ha_btn_state_t s_btn_st[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static bool s_btn_pending[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];   /* tapped, waiting for HA */
#define BTN_ON_COLOR 0xFFC107   /* HA's amber for "on" */
#define BTN_OPA      LV_OPA_40 /* buttons are 60 % transparent */

static void btn_pulse_cb(void *obj, int32_t v) { lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0); }

/* Colours and text for one button from its state; pending buttons pulse */
static void btn_paint(int pg, int i)
{
    lv_obj_t *b = s_btn[pg][i];
    if (!b) return;
    ha_btn_state_t st = s_btn_st[pg][i];
    lv_color_t text = s_light ? lv_color_hex(0x1C1C1E) : lv_color_white();
    lv_color_t bg = lv_color_hex(s_light ? 0xD5DAE0 : 0x3A3F47);
    const char *label = "--";
    switch (st) {
    case HA_BTN_ON:     bg = lv_color_hex(BTN_ON_COLOR); text = lv_color_hex(0x1C1C1E); label = "On"; break;
    case HA_BTN_OFF:    label = "Off"; break;
    case HA_BTN_SCRIPT: label = "Run"; break;
    case HA_BTN_SCENE:  label = "Activate"; break;
    case HA_BTN_PRESS:  label = ""; break;   /* no state line (hidden in create_button) */
    default: break;
    }
    lv_obj_set_style_bg_color(b, bg, 0);
    /* on: dark text on amber; otherwise the card colours (state = value colour, name and icon
     * secondary; unavailable: all secondary) */
    lv_color_t name = st == HA_BTN_ON ? text : COL_SEC(s_light);
    lv_color_t state = st == HA_BTN_ON ? text : st == HA_BTN_UNAVAILABLE ? COL_SEC(s_light) : COL_MAIN(s_light);
    lv_obj_set_style_text_color(s_btn_name[pg][i], name, 0);
    lv_obj_set_style_text_color(s_btn_state[pg][i], state, 0);
    if (s_icon[pg][i]) lv_obj_set_style_img_recolor(s_icon[pg][i], name, 0);
    lv_label_set_text(s_btn_state[pg][i], label);
    bool running = lv_anim_get(b, btn_pulse_cb) != NULL;
    if (s_btn_pending[pg][i] && !running) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, b);
        lv_anim_set_exec_cb(&a, btn_pulse_cb);
        lv_anim_set_values(&a, BTN_OPA, LV_OPA_10);
        lv_anim_set_time(&a, 400);
        lv_anim_set_playback_time(&a, 400);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    } else if (!s_btn_pending[pg][i] && running) {
        lv_anim_del(b, btn_pulse_cb);
        lv_obj_set_style_bg_opa(b, BTN_OPA, 0);
    }
}

/* Short colour flash (scripts/scenes, or red when HA refused a tap) */
static void btn_flash_end(lv_timer_t *t)
{
    int idx = (int)(intptr_t)t->user_data;
    btn_paint(idx / LAYOUT_MAX_ROWS, idx % LAYOUT_MAX_ROWS);
    lv_timer_del(t);
}

static void btn_flash(int pg, int i, uint32_t color)
{
    if (!s_btn[pg][i]) return;
    lv_obj_set_style_bg_color(s_btn[pg][i], lv_color_hex(color), 0);
    lv_timer_t *t = lv_timer_create(btn_flash_end, 400, (void *)(intptr_t)(pg * LAYOUT_MAX_ROWS + i));
    lv_timer_set_repeat_count(t, 1);
}

/* Button card: a rounded translucent rectangle filling its block */
static void create_button(lv_obj_t *page, int pg, int i)
{
    lv_area_t blk;
    graph_block(GI(pg, i), &blk);
    lv_obj_t *b = lv_obj_create(page);
    lv_obj_remove_style_all(b);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_opa(b, BTN_OPA, 0);
    /* full gap at the screen edges, half towards a neighbour (whose half makes it whole) */
    lv_coord_t l = blk.x1 <= 0 ? BTN_INSET : BTN_INSET / 2, r = blk.x2 >= SCR_W - 1 ? BTN_INSET : BTN_INSET / 2;
    lv_coord_t t = blk.y1 <= TOP_M ? BTN_INSET : BTN_INSET / 2, bt = blk.y2 >= SCR_H - 1 - BOTTOM_M ? BTN_INSET : BTN_INSET / 2;
    lv_obj_set_size(b, lv_area_get_width(&blk) - l - r, lv_area_get_height(&blk) - t - bt);
    lv_obj_set_pos(b, blk.x1 + l, blk.y1 + t);
    lv_obj_update_layout(b);
    lv_coord_t inner = lv_obj_get_width(b) - 16;
    const layout_item_t *it = &s_layout.pages[pg].items[i];
    s_btn_name[pg][i] = lv_label_create(b);
    lv_obj_set_style_text_font(s_btn_name[pg][i], &lv_font_latin_16, 0);
    lv_label_set_long_mode(s_btn_name[pg][i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_btn_name[pg][i], inner);
    lv_obj_set_height(s_btn_name[pg][i], lv_font_latin_16.line_height);   /* one line, "..." */
    lv_obj_set_style_text_align(s_btn_name[pg][i], LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_btn_name[pg][i], it->name[0] ? it->name : it->entity);
    s_btn_state[pg][i] = lv_label_create(b);
    lv_obj_set_style_text_font(s_btn_state[pg][i], &lv_font_latin_14, 0);
    /* icon above name and state, all centred as one column: 2x icon when there is room,
     * none when the button is too short for it */
    lv_coord_t h = lv_obj_get_height(b), nh = lv_font_latin_16.line_height, sh = lv_font_latin_14.line_height;
    lv_coord_t text = nh + 2 + sh, ih = 0;
    /* button / input_button entities have no state worth showing: icon and name only */
    if (!strncmp(it->entity, "button.", 7) || !strncmp(it->entity, "input_button.", 13)) {
        text = nh;
        lv_obj_add_flag(s_btn_state[pg][i], LV_OBJ_FLAG_HIDDEN);
    }
    if (icon_slot(pg, i)) {
        if (h - 12 >= text + 4 + 2 * ICON_SIZE && inner >= 2 * ICON_SIZE) ih = 2 * ICON_SIZE;
        else if (h - 8 >= text + 2 + ICON_SIZE) ih = ICON_SIZE;
        else if (h - 8 >= nh + 2 + ICON_SIZE) {   /* short: icon and name, like HA, no state line */
            ih = ICON_SIZE;
            text = nh;
            lv_obj_add_flag(s_btn_state[pg][i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_coord_t gap = ih ? (ih > ICON_SIZE ? 4 : 2) : 0, top = (h - (ih + gap + text)) / 2;
    if (ih) {
        lv_obj_t *o = icon_create(b, pg, i, lv_color_white(), ih);
        lv_obj_align(o, LV_ALIGN_TOP_MID, 0, top);
    }
    lv_obj_align(s_btn_name[pg][i], LV_ALIGN_TOP_MID, 0, top + ih + gap);
    lv_obj_align(s_btn_state[pg][i], LV_ALIGN_TOP_MID, 0, top + ih + gap + nh + 2);
    s_btn[pg][i] = b;
    btn_paint(pg, i);
}

/* ---- Picture cards (picture-entity): the camera/image fitted into the block, name below ---- */
static lv_obj_t *s_pic[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];        /* lv_img */
static lv_obj_t *s_pic_name[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static lv_img_dsc_t s_pic_dsc[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static uint16_t *s_pic_px[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];     /* owned pixels (from ha_client) */

/* Pixel size a picture may take: its block minus the 4 px margin on each side */
static void pic_size(int pg, int i, uint16_t *w, uint16_t *h)
{
    lv_area_t blk;
    graph_block(GI(pg, i), &blk);
    *w = LV_MAX(lv_area_get_width(&blk) - 2 * CARD_INSET, 1);
    *h = LV_MAX(lv_area_get_height(&blk) - 2 * CARD_INSET, 1);
}

/* Tell the network task which page is shown and how big its pictures are (LVGL lock held) */
static void pic_report(void)
{
    uint16_t w[LAYOUT_MAX_ROWS] = {0}, h[LAYOUT_MAX_ROWS] = {0};
    if (PAGE_COUNT && s_page < PAGE_COUNT)
        for (int i = 0; i < ROWS(s_page); i++)
            if (KIND(s_page, i) == LAYOUT_PICTURE) pic_size(s_page, i, &w[i], &h[i]);
    ha_client_show_page(PAGE_COUNT ? s_page : -1, w, h);
}

static void pic_free_all(void)
{
    for (int pg = 0; pg < LAYOUT_MAX_PAGES; pg++)
        for (int i = 0; i < LAYOUT_MAX_ROWS; i++) {
            if (s_pic_px[pg][i]) lv_img_cache_invalidate_src(&s_pic_dsc[pg][i]);
            free(s_pic_px[pg][i]);
        }
    memset(s_pic_px, 0, sizeof(s_pic_px));
    memset(s_pic, 0, sizeof(s_pic));
    memset(s_pic_name, 0, sizeof(s_pic_name));
}

/* Picture name: one line, or two when it doesn't fit (then cut with "...") */
static void pic_name_set(lv_obj_t *n, const char *text)
{
    lv_label_set_text(n, text);
    const lv_font_t *f = lv_obj_get_style_text_font(n, 0);
    lv_obj_update_layout(lv_obj_get_parent(n));
    lv_point_t sz;
    lv_txt_get_size(&sz, text, f, 0, 0, LV_MAX(lv_obj_get_content_width(n), 1), LV_TEXT_FLAG_NONE);
    int lines = sz.y > f->line_height ? 2 : 1;
    lv_obj_set_height(n, lines * f->line_height + 8);
    lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void create_picture(lv_obj_t *page, int pg, int i)
{
    lv_area_t blk;
    graph_block(GI(pg, i), &blk);
    const layout_item_t *it = &s_layout.pages[pg].items[i];
    lv_obj_t *bg = lv_obj_create(page);   /* translucent card panel, picture clipped to it */
    lv_obj_remove_style_all(bg);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(bg, 14, 0);
    lv_obj_set_style_clip_corner(bg, true, 0);
    lv_obj_set_style_bg_opa(bg, BTN_OPA, 0);
    lv_obj_set_style_bg_color(bg, lv_color_hex(s_light ? 0xD5DAE0 : 0x3A3F47), 0);
    lv_obj_set_size(bg, lv_area_get_width(&blk) - 2 * CARD_INSET, lv_area_get_height(&blk) - 2 * CARD_INSET);
    lv_obj_set_pos(bg, blk.x1 + CARD_INSET, blk.y1 + CARD_INSET);
    s_pic[pg][i] = lv_img_create(bg);
    lv_obj_add_flag(s_pic[pg][i], LV_OBJ_FLAG_HIDDEN);   /* until the first image arrives */
    lv_obj_center(s_pic[pg][i]);
    if (it->flags & LAYOUT_PIC_NO_NAME) return;
    lv_obj_t *n = lv_label_create(bg);   /* name on a dark strip along the bottom, like HA */
    lv_obj_set_width(n, LV_PCT(100));
    lv_obj_set_style_pad_all(n, 4, 0);
    lv_obj_set_style_bg_color(n, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(n, LV_OPA_40, 0);
    lv_obj_set_style_text_color(n, lv_color_white(), 0);
    lv_obj_set_style_text_font(n, &lv_font_latin_14, 0);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    pic_name_set(n, it->name[0] ? it->name : it->entity);
    s_pic_name[pg][i] = n;
}

/* Network task: a new picture (or NULL = drop the current one to free memory) */
static void ha_picture_changed(int page, int row, uint16_t *px, int w, int h, int zoom, const char *name)
{
    if (!lvgl_port_lock(0)) { free(px); return; }
    if (page < 0 || page >= PAGE_COUNT || row < 0 || row >= ROWS(page) || KIND(page, row) != LAYOUT_PICTURE || !s_pic[page][row]) {
        lvgl_port_unlock();
        free(px);
        return;
    }
    lv_img_dsc_t *d = &s_pic_dsc[page][row];
    uint16_t *old = s_pic_px[page][row];
    if (old) lv_img_cache_invalidate_src(d);
    if (px) {
        d->header.always_zero = 0;
        d->header.cf = LV_IMG_CF_TRUE_COLOR;
        d->header.w = w;
        d->header.h = h;
        d->data_size = (uint32_t)w * h * sizeof(lv_color_t);
        d->data = (const uint8_t *)px;
        s_pic_px[page][row] = px;
        lv_img_set_src(s_pic[page][row], d);
        lv_img_set_zoom(s_pic[page][row], zoom > 1 ? 256 * zoom : 256);   /* half-size decode */
        lv_img_set_antialias(s_pic[page][row], false);
        lv_obj_center(s_pic[page][row]);
        lv_obj_clear_flag(s_pic[page][row], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_pic[page][row], LV_OBJ_FLAG_HIDDEN);
        lv_img_set_src(s_pic[page][row], NULL);
        s_pic_px[page][row] = NULL;
    }
    free(old);
    if (name && s_pic_name[page][row]) pic_name_set(s_pic_name[page][row], name);
    lvgl_port_unlock();
}

/* ---- Weather cards (weather-forecast: current weather, and the forecast below it when the
 * card has show_forecast: true and the block is tall enough) ---- */
#define FC_H    72                        /* forecast row: time, icon, high, low */
#define FC_MAX  5                         /* at most this many forecast columns */
#define FC_SIDE 12                        /* margin left and right of the forecast row */
#define FC_TOP  8                         /* the current weather sits this much lower with a forecast */
#define FC_BOTTOM 8                       /* gap below the forecast row */
#define WX_PAD 6                          /* side padding inside the current-weather block */
#define WX_SIDE_WIDE 14                   /* extra side margin of a full-width card in landscape */
#define FC_COLW 44                        /* narrowest forecast column: fewer columns in portrait */
typedef struct { int n; bool hourly; ha_forecast_t f[HA_FORECAST_MAX]; } wx_fc_t;
static lv_obj_t *s_fc_box[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static wx_fc_t *s_fc_data[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];   /* last forecast (theme redraws) */
static lv_obj_t *s_wx_icon[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS], *s_wx_cond[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static lv_obj_t *s_wx_name[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS], *s_wx_temp[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static lv_obj_t *s_wx_unit[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS], *s_wx_hum[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static char s_wx_last[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS][24];   /* condition the icon shows */

static const struct { const char *state, *text; } WX_TEXT[] = {
    { "clear-night", "Clear, night" }, { "cloudy", "Cloudy" }, { "exceptional", "Exceptional" },
    { "fog", "Fog" }, { "hail", "Hail" }, { "lightning", "Lightning" }, { "lightning-rainy", "Lightning, rainy" },
    { "partlycloudy", "Partly cloudy" }, { "pouring", "Pouring" }, { "rainy", "Rainy" }, { "snowy", "Snowy" },
    { "snowy-rainy", "Snowy, rainy" }, { "sunny", "Sunny" }, { "windy", "Windy" }, { "windy-variant", "Windy" },
};

static lv_obj_t *wx_shape(lv_obj_t *p, int x, int y, int w, int h, uint32_t col, lv_coord_t radius)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, LV_MAX(w, 1), LV_MAX(h, 1));
    return o;
}
#define WX_DOT(p, x, y, d, col) wx_shape(p, x, y, d, d, col, LV_RADIUS_CIRCLE)

static void wx_cloud(lv_obj_t *p, int S, int x0, int y0, int w, uint32_t col)   /* cloud of width w */
{
    WX_DOT(p, x0 + w * 12 / 100, y0 + w * 22 / 100, w * 40 / 100, col);
    WX_DOT(p, x0 + w * 32 / 100, y0, w * 56 / 100, col);
    wx_shape(p, x0, y0 + w * 34 / 100, w, w * 30 / 100, col, LV_RADIUS_CIRCLE);
    (void)S;
}

static void wx_sun(lv_obj_t *p, int x, int y, int d)
{
    uint32_t sun = s_light ? 0xF2B600 : 0xFDD93C;
    lv_obj_t *s = WX_DOT(p, x, y, d, sun);
    lv_obj_set_style_shadow_color(s, lv_color_hex(sun), 0);
    lv_obj_set_style_shadow_width(s, d / 3, 0);
    lv_obj_set_style_shadow_opa(s, LV_OPA_50, 0);
}

/* Crescent moon as an S x S 8-bit alpha image (4x4 supersampled): the disc minus a
 * second disc shifted up and to the right. Kept per size (the icon box and the forecast). */
static const lv_img_dsc_t *wx_moon(int S)
{
    static lv_img_dsc_t dsc[4];
    static int n;
    for (int i = 0; i < n; i++) if (dsc[i].header.w == S) return &dsc[i];
    lv_img_dsc_t *d = &dsc[n < 4 ? n++ : 3];
    uint8_t *px = (uint8_t *)d->data;
    px = realloc(px, S * S);
    if (!px) return d;
    float c = S / 2.0f, r = S * 0.32f, cx = c + r * 0.42f, cy = c - r * 0.30f, r2 = r * 0.86f;
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            int hit = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float fx = x + (sx + 0.5f) / 4, fy = y + (sy + 0.5f) / 4;
                    float d1 = (fx - c) * (fx - c) + (fy - c) * (fy - c);
                    float d2 = (fx - cx) * (fx - cx) + (fy - cy) * (fy - cy);
                    hit += d1 <= r * r && d2 > r2 * r2;
                }
            px[y * S + x] = (uint8_t)(hit * 255 / 16);
        }
    memset(&d->header, 0, sizeof(d->header));
    d->header.cf = LV_IMG_CF_ALPHA_8BIT;
    d->header.w = S;
    d->header.h = S;
    d->data_size = S * S;
    d->data = px;
    return d;
}

/* Draws the condition's icon into the S x S box p (made of plain shapes, no image data) */
static void wx_draw_icon(lv_obj_t *p, int S, const char *c)
{
    lv_obj_clean(p);
    uint32_t cloud = s_light ? 0x7D8790 : 0xDDE1E5, grey = s_light ? 0x5E6770 : 0xB8BDC2;   /* light: dark enough on the light background */
    uint32_t rain = s_light ? 0x1A8FD6 : 0x30B3FF, snow = s_light ? 0x3E9BD8 : 0xFFFFFF;
    bool rainy = !strcmp(c, "rainy") || !strcmp(c, "pouring") || !strcmp(c, "lightning-rainy") || !strcmp(c, "snowy-rainy");
    bool snowy = !strcmp(c, "snowy") || !strcmp(c, "snowy-rainy"), hail = !strcmp(c, "hail");
    bool bolt = !strncmp(c, "lightning", 9), fog = !strcmp(c, "fog");
    if (!strcmp(c, "sunny")) { wx_sun(p, S / 5, S / 5, S * 3 / 5); return; }
    if (!strcmp(c, "clear-night")) {      /* crescent moon (alpha bitmap, tinted) */
        lv_obj_t *m = lv_img_create(p);
        lv_img_set_src(m, wx_moon(S));
        lv_obj_set_pos(m, 0, 0);
        lv_obj_set_style_img_recolor(m, lv_color_hex(s_light ? 0xC9A400 : 0xF0E6A8), 0);
        lv_obj_set_style_img_recolor_opa(m, LV_OPA_COVER, 0);
        lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE);
        return;
    }
    if (!strncmp(c, "windy", 5)) {
        wx_shape(p, S / 10, S * 3 / 10, S * 6 / 10, S / 10, grey, LV_RADIUS_CIRCLE);
        wx_shape(p, S / 10, S * 5 / 10, S * 8 / 10, S / 10, grey, LV_RADIUS_CIRCLE);
        wx_shape(p, S * 3 / 10, S * 7 / 10, S * 5 / 10, S / 10, grey, LV_RADIUS_CIRCLE);
        return;
    }
    if (!strcmp(c, "exceptional")) {
        lv_obj_t *e = WX_DOT(p, S / 6, S / 6, S * 2 / 3, 0xE53935);
        lv_obj_t *l = lv_label_create(e);
        lv_label_set_text(l, "!");
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_22, 0);
        lv_obj_center(l);
        return;
    }
    if (!strcmp(c, "partlycloudy")) {
        wx_sun(p, S * 42 / 100, S * 8 / 100, S / 2);
        wx_cloud(p, S, S * 4 / 100, S * 36 / 100, S * 76 / 100, cloud);
        return;
    }
    bool below = rainy || snowy || hail || bolt || fog;   /* cloud moves up to make room */
    wx_cloud(p, S, S / 10, below ? S * 6 / 100 : S / 5, S * 8 / 10, cloud);
    int y = S * 62 / 100;
    if (fog) {
        wx_shape(p, S * 18 / 100, y + S / 20, S * 64 / 100, S / 12, grey, LV_RADIUS_CIRCLE);
        wx_shape(p, S * 18 / 100, y + S * 2 / 10, S * 64 / 100, S / 12, grey, LV_RADIUS_CIRCLE);
        return;
    }
    if (bolt) {
        lv_obj_t *b = lv_label_create(p);
        lv_label_set_text(b, LV_SYMBOL_CHARGE);
        lv_obj_set_style_text_color(b, lv_color_hex(0xFDD93C), 0);
        lv_obj_set_style_text_font(b, S >= 44 ? &lv_font_montserrat_20 : &lv_font_montserrat_16, 0);
        lv_obj_set_pos(b, S * 42 / 100, y - S / 20);
    }
    int n = !strcmp(c, "pouring") ? 5 : 3, dw = LV_MAX(S / 16, 2);
    for (int k = 0; k < n; k++) {
        int x = S * 22 / 100 + k * (S * 56 / 100) / (n - 1) - dw / 2;
        if (bolt && k == 1) continue;     /* the bolt sits in the middle */
        bool flake = snowy && (!rainy || k % 2 == 0);
        if (hail) WX_DOT(p, x - dw / 2, y + S / 10 + (k % 2) * S / 10, dw * 2, s_light ? 0x6F8BA3 : 0xFFFFFF);
        else if (flake) WX_DOT(p, x - dw / 2, y + S / 10 + (k % 2) * S / 10, dw * 2, snow);
        else if (rainy) wx_shape(p, x, y + S / 12 + (k % 2) * S / 12, dw, S / 5, rain, LV_RADIUS_CIRCLE);
    }
}

static void create_weather(lv_obj_t *page, int pg, int i)
{
    lv_area_t blk;
    graph_block(GI(pg, i), &blk);
    int w = lv_area_get_width(&blk) - 2 * CARD_INSET, h = lv_area_get_height(&blk) - 2 * CARD_INSET;
    int side = s_landscape && w >= SCR_W - 2 * CARD_INSET ? WX_SIDE_WIDE : 0;   /* full-width card: more room at the sides */
    /* the current weather on top sits closer to the sides than the forecast row: 60 % */
    int side_t = side * 6 / 10, w_t = w - 2 * side_t;
    w -= 2 * side;
    s_fc_box[pg][i] = NULL;
    if ((s_layout.pages[pg].items[i].flags & LAYOUT_WX_FORECAST) && h - FC_H - FC_BOTTOM - FC_TOP >= (w < 240 ? 100 : 64)) {
        h -= FC_H + FC_BOTTOM + FC_TOP;   /* current weather on top (moved down a little), forecast row below */
        lv_obj_t *fb = lv_obj_create(page);
        lv_obj_remove_style_all(fb);
        lv_obj_clear_flag(fb, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(fb, w, FC_H);
        lv_obj_set_pos(fb, blk.x1 + side + CARD_INSET, blk.y1 + CARD_INSET + FC_TOP + h);
        s_fc_box[pg][i] = fb;
        blk.y1 += FC_TOP;
    }
    lv_obj_t *bg = lv_obj_create(page);   /* invisible container, no card background */
    lv_obj_remove_style_all(bg);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    w = w_t;
    lv_obj_set_size(bg, w, h);
    lv_obj_set_pos(bg, blk.x1 + side_t + CARD_INSET, blk.y1 + CARD_INSET);
    lv_color_t fg = COL_MAIN(s_light), sec = COL_SEC(s_light);
    bool stacked = w < 240 && h >= 96;    /* portrait: icon + temperature on top, text below */
    int S = stacked ? LV_MIN(52, h / 2) : LV_MIN(52, h - 16), pad = WX_PAD;

    lv_obj_t *ic = lv_obj_create(bg);
    lv_obj_remove_style_all(ic);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ic, S, S);

    lv_obj_t *t = lv_label_create(bg), *u = lv_label_create(bg), *hm = lv_label_create(bg);
    lv_label_set_text(t, "--");
    lv_label_set_text(u, "");
    lv_label_set_text(hm, "");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_font(u, &lv_font_latin_16, 0);
    lv_obj_set_style_text_font(hm, &lv_font_montserrat_14, 0);   /* has the drop symbol */
    lv_obj_set_style_text_color(t, fg, 0);
    lv_obj_set_style_text_color(u, sec, 0);
    lv_obj_set_style_text_color(hm, sec, 0);

    lv_obj_t *c = lv_label_create(bg), *n = lv_label_create(bg);
    lv_label_set_text(c, "");
    lv_label_set_text(n, s_layout.pages[pg].items[i].name[0] ? s_layout.pages[pg].items[i].name : s_layout.pages[pg].items[i].entity);
    lv_obj_set_style_text_font(c, &lv_font_latin_22, 0);
    lv_obj_set_style_text_font(n, &lv_font_latin_14, 0);
    lv_obj_set_style_text_color(c, fg, 0);
    lv_obj_set_style_text_color(n, sec, 0);
    lv_label_set_long_mode(c, LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);

    if (stacked) {
        int top = (h - S - 8 - 26 - 18) / 2;   /* icon row, condition, name: centred */
        if (top < 4) top = 4;
        lv_obj_set_pos(ic, pad - 4, top);
        lv_obj_set_width(c, w - 2 * pad);
        lv_obj_set_width(n, w - 2 * pad);
        lv_obj_set_pos(c, pad, top + S + 6);
        lv_obj_set_pos(n, pad, top + S + 6 + 26);
        lv_obj_set_user_data(bg, (void *)(intptr_t)top);
    } else {
        lv_obj_set_pos(ic, pad - 2, (h - S) / 2);
        int tx = pad + S + 8, tw = w - tx - pad - 90;   /* temperature takes about 90 px */
        lv_obj_set_width(c, LV_MAX(tw, 20));
        lv_obj_set_width(n, LV_MAX(tw, 20));
        lv_obj_set_pos(c, tx, h / 2 - 25);
        lv_obj_set_pos(n, tx, h / 2 + 1);
        lv_obj_set_user_data(bg, (void *)(intptr_t)-1);
    }
    s_wx_icon[pg][i] = ic; s_wx_cond[pg][i] = c; s_wx_name[pg][i] = n;
    s_wx_temp[pg][i] = t; s_wx_unit[pg][i] = u; s_wx_hum[pg][i] = hm;
    s_wx_last[pg][i][0] = 0;
}

/* Temperature (number + unit) and humidity, right-aligned */
static void wx_place(int pg, int i)
{
    lv_obj_t *t = s_wx_temp[pg][i], *u = s_wx_unit[pg][i], *hm = s_wx_hum[pg][i], *bg = lv_obj_get_parent(t);
    lv_obj_update_layout(bg);
    int w = lv_obj_get_width(bg), h = lv_obj_get_height(bg), pad = WX_PAD;
    int top = (int)(intptr_t)lv_obj_get_user_data(bg);
    if (top >= 0) {                       /* stacked: centre icon, condition and name (1-2 lines) */
        lv_obj_t *ic = s_wx_icon[pg][i], *c = s_wx_cond[pg][i], *n = s_wx_name[pg][i];
        int S = lv_obj_get_height(ic), ch = lv_obj_get_height(c), nh = lv_obj_get_height(n);
        top = LV_MAX((h - (S + 6 + ch + nh)) / 2, 0);
        lv_obj_set_y(ic, top);
        lv_obj_set_y(c, top + S + 6);
        lv_obj_set_y(n, top + S + 6 + ch);
    }
    int tw = lv_obj_get_width(t), uw = lv_obj_get_width(u), th = lv_obj_get_height(t);
    int y = top >= 0 ? top + 2 : h / 2 - th + 4;
    lv_obj_set_pos(u, w - pad - uw, y + 3);
    lv_obj_set_pos(t, w - pad - uw - 3 - tw, y);
    lv_obj_set_pos(hm, w - pad - lv_obj_get_width(hm), y + th + 1);
}

static void ha_weather_changed(int page, int row, const char *cond, const char *temp, const char *unit,
                               const char *hum, const char *name)
{
    if (!lvgl_port_lock(0)) return;
    if (page < 0 || page >= PAGE_COUNT || row < 0 || row >= ROWS(page) || KIND(page, row) != LAYOUT_WEATHER || !s_wx_temp[page][row]) {
        lvgl_port_unlock();
        return;
    }
    const char *text = cond[0] ? cond : "Unavailable";
    for (size_t k = 0; k < sizeof(WX_TEXT) / sizeof(WX_TEXT[0]); k++)
        if (!strcmp(cond, WX_TEXT[k].state)) text = WX_TEXT[k].text;
    lv_label_set_text(s_wx_cond[page][row], text);
    if (name) lv_label_set_text(s_wx_name[page][row], name);
    lv_label_set_text(s_wx_temp[page][row], temp[0] ? temp : "--");
    lv_label_set_text(s_wx_unit[page][row], unit);
    if (hum[0]) lv_label_set_text_fmt(s_wx_hum[page][row], LV_SYMBOL_TINT " %s%%", hum);   /* drop */
    else lv_label_set_text(s_wx_hum[page][row], "");
    if (strcmp(s_wx_last[page][row], cond)) {
        snprintf(s_wx_last[page][row], sizeof(s_wx_last[page][row]), "%s", cond);
        wx_draw_icon(s_wx_icon[page][row], lv_obj_get_width(s_wx_icon[page][row]), cond);
    }
    wx_place(page, row);
    lvgl_port_unlock();
}

/* Forecast columns: as many as fit (at least FC_COLW wide each), drawn from s_fc_data */
static void wx_draw_forecast(int pg, int i)
{
    lv_obj_t *fb = s_fc_box[pg][i];
    const wx_fc_t *d = s_fc_data[pg][i];
    if (!fb) return;
    lv_obj_clean(fb);
    if (!d || d->n <= 0) return;
    int w = lv_obj_get_width(fb) - 2 * FC_SIDE, cols = LV_MIN(LV_MIN(d->n, FC_MAX), LV_MAX(w / FC_COLW, 1)), cw = w / cols;
    lv_color_t fg = COL_MAIN(s_light), sec = COL_SEC(s_light);
    for (int k = 0; k < cols; k++) {
        const ha_forecast_t *f = &d->f[k];
        int x = FC_SIDE + k * cw;
        char tl[12];
        time_t tt = (time_t)f->t;
        struct tm tm;
        localtime_r(&tt, &tm);
        strftime(tl, sizeof(tl), d->hourly ? "%H:%M" : "%a", &tm);
        lv_obj_t *l = lv_label_create(fb);
        lv_label_set_text(l, tl);
        lv_obj_set_style_text_font(l, &lv_font_latin_12, 0);
        lv_obj_set_style_text_color(l, sec, 0);
        lv_obj_set_width(l, cw);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, x, 0);
        lv_obj_t *ic = lv_obj_create(fb);
        lv_obj_remove_style_all(ic);
        lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(ic, 26, 26);
        lv_obj_set_pos(ic, x + (cw - 26) / 2, 15);
        wx_draw_icon(ic, 26, f->cond);
        lv_obj_t *hi = lv_label_create(fb), *lo = lv_label_create(fb);
        lv_label_set_text_fmt(hi, "%s°", f->hi[0] ? f->hi : "--");
        if (f->lo[0]) lv_label_set_text_fmt(lo, "%s°", f->lo);
        else lv_label_set_text(lo, "");
        lv_obj_set_style_text_font(hi, &lv_font_latin_16, 0);
        lv_obj_set_style_text_font(lo, &lv_font_latin_12, 0);
        lv_obj_set_style_text_color(hi, fg, 0);
        lv_obj_set_style_text_color(lo, sec, 0);
        lv_obj_t *t2[] = { hi, lo };
        for (int m = 0; m < 2; m++) {
            lv_obj_set_width(t2[m], cw);
            lv_obj_set_style_text_align(t2[m], LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(t2[m], LV_LABEL_LONG_CLIP);
        }
        lv_obj_set_pos(hi, x, 42);
        lv_obj_set_pos(lo, x, 58);
    }
}

static void ha_forecast_changed(int page, int row, const ha_forecast_t *f, int n, bool hourly)
{
    if (!lvgl_port_lock(0)) return;
    if (page < 0 || page >= PAGE_COUNT || row < 0 || row >= ROWS(page) || KIND(page, row) != LAYOUT_WEATHER || !s_fc_box[page][row]) {
        lvgl_port_unlock();
        return;
    }
    wx_fc_t *d = s_fc_data[page][row];
    if (!d) d = s_fc_data[page][row] = malloc(sizeof(*d));
    if (d) {
        d->n = LV_MIN(n, HA_FORECAST_MAX);
        d->hourly = hourly;
        memcpy(d->f, f, d->n * sizeof(*f));
        wx_draw_forecast(page, row);
    }
    lvgl_port_unlock();
}

/* ---- Gauge cards: the value on a half-circle arc, like HA's gauge card ---- */
static lv_obj_t *s_gz_arc[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS], *s_gz_val[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static lv_obj_t *s_gz_unit[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS], *s_gz_name[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];
static lv_coord_t s_gz_cx[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS], s_gz_flat[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];   /* flat = value baseline */
#define GZ_SCALE 1000

static void gauge_place(int pg, int i)
{
    lv_obj_t *v = s_gz_val[pg][i], *u = s_gz_unit[pg][i];
    lv_obj_update_layout(v);
    lv_obj_update_layout(u);
    const lv_font_t *vf = lv_obj_get_style_text_font(v, 0), *uf = lv_obj_get_style_text_font(u, 0);
    lv_coord_t vw = lv_obj_get_width(v), uw = lv_label_get_text(u)[0] ? lv_obj_get_width(u) + 3 : 0;
    lv_coord_t x = s_gz_cx[pg][i] - (vw + uw) / 2, base = s_gz_flat[pg][i] - 2;   /* baseline on the flat side */
    lv_obj_set_pos(v, x, base - (vf->line_height - vf->base_line));
    lv_obj_set_pos(u, x + vw + 3, base - (uf->line_height - uf->base_line));
}

/* Arc, value and name sized to the block; the name takes one or two lines (then the arc
 * gets smaller), ending in "..." only after the second */
static void gauge_layout(int pg, int i)
{
    lv_area_t blk;
    graph_block(GI(pg, i), &blk);
    int w = lv_area_get_width(&blk), h = lv_area_get_height(&blk);
    lv_obj_t *a = s_gz_arc[pg][i], *n = s_gz_name[pg][i];
    const lv_font_t *nf = lv_obj_get_style_text_font(n, 0);
    lv_point_t sz;
    lv_txt_get_size(&sz, lv_label_get_text(n), nf, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    /* two lines only between words: a single word too wide ends in "..." instead of
     * breaking inside it */
    const char *t = lv_label_get_text(n);
    bool wrap = sz.x > w - 4 && strchr(t, ' ');
    for (const char *p = t; wrap && *p; ) {   /* every word must fit a line */
        size_t l = strcspn(p, " ");
        char word[40];
        snprintf(word, sizeof(word), "%.*s", (int)LV_MIN(l, sizeof(word) - 1), p);
        lv_point_t ws;
        lv_txt_get_size(&ws, word, nf, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (ws.x > w - 4) wrap = false;
        p += l;
        while (*p == ' ') p++;
    }
    lv_coord_t nh = nf->line_height * (wrap ? 2 : 1);
    /* landscape: the value's digits centred on the block, the flat edge of the (small) arc
     * through the value's middle; portrait: arc, value and name centred as a whole, the
     * value's baseline on the flat edge */
    lv_coord_t cy = blk.y1 + h / 2, cx = blk.x1 + w / 2;
    const lv_font_t *vf = &lv_font_montserrat_28;
    lv_coord_t D = 0, base = 0, flat = 0;
    for (int pass = 0; pass < 3; pass++) {
        lv_font_glyph_dsc_t g;
        lv_coord_t dh = lv_font_get_glyph_dsc(vf, &g, '0', 0) ? g.box_h : vf->line_height / 2;
        if (s_landscape) {                /* centred on the value, which sits lower */
            base = cy + dh / 2;
            flat = cy;
            D = LV_MAX(LV_MIN(w - 24, 2 * (flat - blk.y1 - 4)), 40);
        } else {                          /* arc, value and name centred as a whole */
            D = LV_MAX(LV_MIN(w - 24, 2 * (h - nh - 6 - 2 * 12)), 40);   /* 12 px above and below */
            flat = blk.y1 + (h - (D / 2 + 6 + nh)) / 2 + D / 2;
            base = flat;
        }
        const lv_font_t *want = D >= 120 ? &lv_font_montserrat_28 : D >= 96 ? &lv_font_montserrat_20 : &lv_font_montserrat_16;
        if (want == vf) break;
        vf = want;
    }
    lv_obj_set_size(a, D, D);
    lv_obj_set_pos(a, cx - D / 2, flat - D / 2);
    lv_obj_set_style_arc_width(a, LV_MAX(D / 8, 6), LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, LV_MAX(D / 8, 6), LV_PART_INDICATOR);
    s_gz_cx[pg][i] = cx;
    s_gz_flat[pg][i] = base + 2;          /* gauge_place puts the baseline 2 px above this */
    if (!s_crisp[GI(pg, i)]) lv_obj_set_style_text_font(s_gz_val[pg][i], vf, 0);
    lv_obj_set_size(n, w - 4, nh);
    lv_obj_set_pos(n, blk.x1 + 2, base + 6);
    gauge_place(pg, i);
}

static void create_gauge(lv_obj_t *page, int pg, int i)
{
    lv_obj_t *a = lv_arc_create(page);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_rotation(a, 180);
    lv_arc_set_bg_angles(a, 0, 180);
    lv_arc_set_range(a, 0, GZ_SCALE);
    lv_arc_set_value(a, 0);
    lv_obj_set_style_pad_all(a, 0, 0);
    lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, false, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(a, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, COL_SEC(s_light), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(0x03A9F4), LV_PART_INDICATOR);   /* HA's primary */
    s_gz_arc[pg][i] = a;

    lv_obj_t *v = lv_label_create(page), *u = lv_label_create(page), *n = lv_label_create(page);
    lv_obj_set_style_text_font(u, &lv_font_latin_14, 0);
    lv_obj_set_style_text_font(n, &lv_font_latin_16, 0);
    lv_obj_set_style_text_color(v, COL_MAIN(s_light), 0);
    lv_obj_set_style_text_color(u, COL_SEC(s_light), 0);
    lv_obj_set_style_text_color(n, COL_SEC(s_light), 0);
    lv_label_set_text(v, "--");
    lv_label_set_text(u, "");
    const layout_item_t *it = &s_layout.pages[pg].items[i];
    lv_label_set_text(n, it->name[0] ? it->name : it->entity);
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    s_gz_val[pg][i] = v; s_gz_unit[pg][i] = u; s_gz_name[pg][i] = n;
    gauge_layout(pg, i);
}

static void gauge_update(int pg, int i, const char *value, const char *unit, const char *name)
{
    const layout_item_t *it = &s_layout.pages[pg].items[i];
    int g = GI(pg, i);
    bool crisp = s_crisp[g];
    crisp_off(g);
    lv_label_set_text(s_gz_val[pg][i], value);
    lv_label_set_text(s_gz_unit[pg][i], unit);
    if (!it->name[0] && name && strcmp(lv_label_get_text(s_gz_name[pg][i]), name)) {
        lv_label_set_text(s_gz_name[pg][i], name);
        gauge_layout(pg, i);              /* one or two name lines */
    }
    gauge_place(pg, i);
    page_relayout(pg);                    /* re-applies a tapped gauge's growth */
    if (crisp) crisp_on(g);
    char *end;
    float v = strtof(value, &end);
    if (end == value) { lv_arc_set_value(s_gz_arc[pg][i], 0); return; }
    float f = (v - it->min) / (it->max - it->min);
    lv_arc_set_value(s_gz_arc[pg][i], (int16_t)(LV_CLAMP(0.0f, f, 1.0f) * GZ_SCALE));
    uint32_t col = 0x03A9F4;              /* severity: the highest threshold reached, like HA */
    static const uint32_t SEV[3] = { 0x43A047, 0xFFA600, 0xDB4437 };
    for (int k = 0; k < 3; k++) if (!isnan(it->sev[k]) && v >= it->sev[k]) col = SEV[k];
    lv_obj_set_style_arc_color(s_gz_arc[pg][i], lv_color_hex(col), LV_PART_INDICATOR);
}

/* ---- Glance cards: one cell per entity, icon over name over state ---- */
static lv_obj_t *s_gl_name[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS], *s_gl_state[LAYOUT_MAX_PAGES][LAYOUT_MAX_ROWS];

static void create_glance(lv_obj_t *page, int pg, int i)
{
    lv_area_t blk;
    graph_block(GI(pg, i), &blk);
    int w = lv_area_get_width(&blk), h = lv_area_get_height(&blk);
    const layout_item_t *it = &s_layout.pages[pg].items[i];
    bool name = !(it->flags & LAYOUT_GLANCE_NO_NAME), state = !(it->flags & LAYOUT_GLANCE_NO_STATE);
    const lv_font_t *nf = &lv_font_latin_14, *sf = &lv_font_latin_16;
    lv_coord_t text = (name ? nf->line_height + 2 : 0) + (state ? sf->line_height : 0), ih = 0;
    if (icon_slot(pg, i)) ih = h - 16 >= text + 4 + 2 * ICON_SIZE && w - 16 >= 2 * ICON_SIZE ? 2 * ICON_SIZE
                             : h - 8 >= text + 2 + ICON_SIZE ? ICON_SIZE : 0;
    lv_coord_t gap = ih ? 4 : 0, y = blk.y1 + (h - (ih + gap + text)) / 2;
    if (ih) {
        lv_obj_t *o = icon_create(page, pg, i, icon_color(), ih);
        lv_obj_set_pos(o, blk.x1 + (w - ih) / 2, y);
        y += ih + gap;
    }
    lv_obj_t *labels[2] = { NULL, NULL };   /* [0] name, [1] state */
    for (int j = 0; j < 2; j++) {
        int k = 1 - j;                    /* state (value) first, name below it */
        if (k == 0 ? !name : !state) continue;
        lv_obj_t *l = lv_label_create(page);
        const lv_font_t *f = k == 0 ? nf : sf;
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_style_text_color(l, k == 0 ? COL_SEC(s_light) : COL_MAIN(s_light), 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_size(l, w - 8, f->line_height);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, blk.x1 + 4, y);
        lv_label_set_text(l, k == 0 ? (it->name[0] ? it->name : it->entity) : "--");
        y += f->line_height + 2;
        labels[k] = l;
    }
    s_gl_name[pg][i] = labels[0];
    s_gl_state[pg][i] = labels[1];
}

static void glance_update(int pg, int i, const char *value, const char *unit, const char *name)
{
    if (s_gl_state[pg][i]) lv_label_set_text_fmt(s_gl_state[pg][i], "%s%s%s", value, unit[0] ? " " : "", unit);
    if (s_gl_name[pg][i] && !s_layout.pages[pg].items[i].name[0] && name) lv_label_set_text(s_gl_name[pg][i], name);
}

/* ---- Clock pages: the board's time (set from HA's responses), updated every second ---- */
/* one set per block, indexed GI(page, row) */
static lv_obj_t *s_clk_time[GRAPHS], *s_clk_small[GRAPHS];   /* no date, like HA's card */
static lv_obj_t *s_clk_bg[GRAPHS];   /* card background (unless no_background) */
static lv_timer_t *s_clk_timer;

static void clock_update(int gi)
{
    if (!s_clk_time[gi]) return;
    const layout_item_t *it = &s_layout.pages[GPG(gi)].items[GROW(gi)];
    uint8_t fl = it->flags;
    time_t now = time(NULL);
    struct tm t;
    /* the card's time_zone, else the board's (HA_TIMEZONE) */
    const char *zone = it->entity[0] ? tz_posix(it->entity) : NULL;
    if (zone) { setenv("TZ", zone, 1); tzset(); }
    localtime_r(&now, &t);
    if (zone) { setenv("TZ", HA_TIMEZONE, 1); tzset(); }
    char hm[8], small[8] = "";
    if (t.tm_year < 120) {                 /* before 2020: no time from HA yet */
        strcpy(hm, "--:--");
    } else {
        int h = t.tm_hour;
        if (fl & LAYOUT_CLOCK_12H) h = h % 12 ? h % 12 : 12;
        snprintf(hm, sizeof(hm), "%02d:%02d", h, t.tm_min);   /* zero padded: 07:05 */
        /* seconds and AM/PM go small beside the time: ":05 PM" */
        int n = 0;
        if (fl & LAYOUT_CLOCK_SECONDS) n = snprintf(small, sizeof(small), ":%02d", t.tm_sec);
        if (fl & LAYOUT_CLOCK_12H) snprintf(small + n, sizeof(small) - n, "%s%s", n ? " " : "", t.tm_hour < 12 ? "AM" : "PM");
    }
    lv_label_set_text(s_clk_time[gi], hm);
    lv_label_set_text(s_clk_small[gi], small);
    /* centre time + seconds/AM-PM together in the block */
    lv_area_t blk;
    graph_block(gi, &blk);
    lv_obj_update_layout(s_clk_time[gi]);
    lv_obj_update_layout(s_clk_small[gi]);
    lv_coord_t tw = lv_obj_get_width(s_clk_time[gi]), sw = small[0] ? lv_obj_get_width(s_clk_small[gi]) + 4 : 0;
    lv_coord_t th = lv_obj_get_height(s_clk_time[gi]);
    lv_obj_set_pos(s_clk_time[gi], blk.x1 + (lv_area_get_width(&blk) - tw - sw) / 2,
                   blk.y1 + (lv_area_get_height(&blk) - th) / 2);
    lv_obj_align_to(s_clk_small[gi], s_clk_time[gi], LV_ALIGN_OUT_RIGHT_BOTTOM, 4,
                    -(lv_obj_get_style_text_font(s_clk_time[gi], 0)->base_line -
                      lv_obj_get_style_text_font(s_clk_small[gi], 0)->base_line));
}

static void clock_tick(lv_timer_t *t)
{
    (void)t;
    for (int pg = 0; pg < PAGE_COUNT; pg++)
        for (int i = 0; i < ROWS(pg); i++)
            if (KIND(pg, i) == LAYOUT_CLOCK) clock_update(GI(pg, i));
}

/* clock_size small/medium/large = 22/48/72 px digits, smaller (72, 48, 36, 22) while the
 * time doesn't fit its block */
/* clock fonts, ascending: the text fonts below 24 px, then digits-only fonts (tools/make_fonts.py) */
static const lv_font_t *const CLOCK_FONTS[] = {
    &lv_font_latin_12, &lv_font_latin_14, &lv_font_latin_16, &lv_font_latin_22,   /* small steps */
    &lv_font_clock_24, &lv_font_clock_28, &lv_font_clock_32, &lv_font_clock_36, &lv_font_clock_40,
    &lv_font_clock_44, &lv_font_clock_48, &lv_font_clock_56, &lv_font_clock_64, &lv_font_clock_72,
};
static const uint8_t CLOCK_PX[] = { 12, 14, 16, 22, 24, 28, 32, 36, 40, 44, 48, 56, 64, 72 };
#define CLOCK_N (int)(sizeof(CLOCK_PX) / sizeof(CLOCK_PX[0]))

/* seconds / AM-PM beside the time, scaled with it */
static const lv_font_t *clock_small_font(const lv_font_t *time_font)
{
    int lh = time_font->line_height;
    return lh >= lv_font_clock_48.line_height ? &lv_font_latin_22
         : lh >= lv_font_clock_32.line_height ? &lv_font_latin_16 : &lv_font_latin_12;
}

static bool clock_fits(int i, uint8_t flags, lv_coord_t max_w, lv_coord_t max_h)
{
    const char *small = (flags & LAYOUT_CLOCK_SECONDS) && (flags & LAYOUT_CLOCK_12H) ? ":00 PM"
                      : flags & LAYOUT_CLOCK_SECONDS ? ":00" : flags & LAYOUT_CLOCK_12H ? "PM" : "";
    lv_point_t sz, extra = { 0, 0 };
    lv_txt_get_size(&sz, "00:00", CLOCK_FONTS[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (small[0]) lv_txt_get_size(&extra, small, clock_small_font(CLOCK_FONTS[i]), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x + (extra.x ? extra.x + 4 : 0) <= max_w && CLOCK_FONTS[i]->line_height <= max_h;
}

/* clock_size small/medium/large = 40/56/72 px. When a large clock wouldn't fit its block,
 * all three shrink by the same factor, so large stays bigger than medium and small. */
static const lv_font_t *clock_font(uint8_t flags, lv_coord_t max_w, lv_coord_t max_h)
{
    int target = (flags & LAYOUT_CLOCK_SIZE) == LAYOUT_CLOCK_LARGE ? 72 : (flags & LAYOUT_CLOCK_SIZE) == LAYOUT_CLOCK_MEDIUM ? 56 : 40;
    int fit = 0;   /* largest size that fits, measured with this clock's seconds/AM-PM */
    for (int i = CLOCK_N - 1; i >= 0; i--) if (clock_fits(i, flags, max_w, max_h)) { fit = i; break; }
    int want = target * CLOCK_PX[fit] / 72;
    int pick = 0;
    for (int i = 0; i < CLOCK_N; i++) if (CLOCK_PX[i] <= want) pick = i;
    return CLOCK_FONTS[pick];
}

static void create_clock(lv_obj_t *page, int pg, int i)
{
    int gi = GI(pg, i);
    const layout_item_t *it = &s_layout.pages[pg].items[i];
    lv_area_t blk;
    graph_block(gi, &blk);
    if (!(it->flags & LAYOUT_CLOCK_NO_BG)) {   /* translucent card panel */
        lv_obj_t *bg = lv_obj_create(page);
        lv_obj_remove_style_all(bg);
        lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(bg, 14, 0);
        lv_obj_set_style_bg_opa(bg, BTN_OPA, 0);     /* 60 % transparent, like the buttons */
        lv_obj_set_size(bg, lv_area_get_width(&blk) - 2 * CARD_INSET, lv_area_get_height(&blk) - 2 * CARD_INSET);
        lv_obj_set_pos(bg, blk.x1 + CARD_INSET, blk.y1 + CARD_INSET);
        s_clk_bg[gi] = bg;
    }
    if (it->entity[0] && !tz_posix(it->entity))
        ESP_LOGW("clock", "time zone '%s' not known, using the board's", it->entity);
    s_clk_time[gi] = lv_label_create(page);
    const lv_font_t *tf = clock_font(it->flags, lv_area_get_width(&blk) - 24, lv_area_get_height(&blk) - 8);
    lv_obj_set_style_text_font(s_clk_time[gi], tf, 0);
    s_clk_small[gi] = lv_label_create(page);
    lv_obj_set_style_text_font(s_clk_small[gi], clock_small_font(tf), 0);
    if (!s_clk_timer) s_clk_timer = lv_timer_create(clock_tick, 1000, NULL);
    clock_update(gi);
}

/* A tap on a button page: toggle at once (optimistic), HA confirms on the next poll */
static bool button_tap(lv_point_t p)
{
    int pg = s_page;
    if (pg >= PAGE_COUNT) return false;
    for (int i = 0; i < ROWS(pg); i++) {
        lv_area_t a;
        if (KIND(pg, i) != LAYOUT_BUTTONS || !s_btn[pg][i]) continue;
        lv_obj_get_coords(s_btn[pg][i], &a);
        if (p.x < a.x1 || p.x > a.x2 || p.y < a.y1 || p.y > a.y2) continue;
        ha_btn_state_t st = s_btn_st[pg][i];
        if (st == HA_BTN_UNAVAILABLE || s_layout.pages[pg].items[i].action != LAYOUT_ACT_TOGGLE) return true;
        if (st == HA_BTN_ON || st == HA_BTN_OFF) {
            s_btn_st[pg][i] = st == HA_BTN_ON ? HA_BTN_OFF : HA_BTN_ON;
            s_btn_pending[pg][i] = true;
            btn_paint(pg, i);
        } else {
            btn_flash(pg, i, BTN_ON_COLOR);
        }
        ha_client_press(pg, i);
        return true;
    }
    return false;   /* not on a button: other blocks handle the tap */
}

/* Network task: state of one button (a poll confirms or corrects a pending tap) */
static void ha_button_changed(int page, int row, ha_btn_state_t state, const char *name)
{
    if (!lvgl_port_lock(0)) return;
    if (page >= 0 && page < PAGE_COUNT && row >= 0 && row < ROWS(page) && KIND(page, row) == LAYOUT_BUTTONS &&
        s_btn[page][row]) {
        s_btn_st[page][row] = state;
        s_btn_pending[page][row] = false;
        if (strcmp(lv_label_get_text(s_btn_name[page][row]), name)) lv_label_set_text(s_btn_name[page][row], name);
        btn_paint(page, row);
    }
    lvgl_port_unlock();
}

/* Network task: HA's answer to a tap; a refused toggle flips back and flashes red */
static void ha_button_pressed(int page, int row, bool ok)
{
    if (!lvgl_port_lock(0)) return;
    if (!ok && page >= 0 && page < PAGE_COUNT && row >= 0 && row < ROWS(page) && KIND(page, row) == LAYOUT_BUTTONS &&
        s_btn[page][row]) {
        ha_btn_state_t st = s_btn_st[page][row];
        if (s_btn_pending[page][row]) s_btn_st[page][row] = st == HA_BTN_ON ? HA_BTN_OFF : HA_BTN_ON;
        s_btn_pending[page][row] = false;
        btn_paint(page, row);
        btn_flash(page, row, 0xE74C3C);
    }
    lvgl_port_unlock();
}

static void apply_temp_theme(bool light)
{
    lv_color_t fg = light ? lv_color_hex(0x1C1C1E) : lv_color_white();   /* page dots */
    lv_color_t sub = COL_SEC(light), val = COL_MAIN(light), unit = sub;
    for (int pg = 0; pg < LAYOUT_MAX_PAGES; pg++) {
        for (int i = 0; i < LAYOUT_MAX_ROWS; i++) {
            if (!s_val[pg][i]) continue;
            lv_obj_set_style_text_color(s_name[pg][i], sub, 0);
            lv_obj_set_style_text_color(s_val[pg][i], val, 0);
            lv_obj_set_style_text_color(s_unit[pg][i], unit, 0);
        }
    }
    for (int pg = 0; pg < LAYOUT_MAX_PAGES; pg++)
        for (int i = 0; i < LAYOUT_MAX_ROWS; i++)
            if (s_icon[pg][i] && pg < PAGE_COUNT && i < ROWS(pg) && KIND(pg, i) != LAYOUT_BUTTONS)
                lv_obj_set_style_img_recolor(s_icon[pg][i], sub, 0);
    for (s_gi = 0; s_gi < GRAPHS; s_gi++) {
        if (!s_chart) continue;
        lv_obj_set_style_text_color(s_g_name, sub, 0);
        lv_obj_set_style_text_color(s_g_val, val, 0);
        lv_obj_set_style_text_color(s_g_unit, unit, 0);
        lv_chart_set_series_color(s_chart, s_chart_ser, lv_color_hex(light ? 0x2D8EC7 : 0x0E6FA8));   /* like HA's sensor card */
    }
    s_gi = 0;
    for (int pg = 0; pg < LAYOUT_MAX_PAGES; pg++) {
        for (int i = 0; i < LAYOUT_MAX_ROWS; i++) btn_paint(pg, i);
        for (int i = 0; i < LAYOUT_MAX_ROWS; i++) {
            int gi = GI(pg, i);
            if (s_clk_bg[gi]) lv_obj_set_style_bg_color(s_clk_bg[gi], lv_color_hex(light ? 0xD5DAE0 : 0x3A3F47), 0);
            if (s_clk_time[gi]) {
                lv_obj_set_style_text_color(s_clk_time[gi], val, 0);
                lv_obj_set_style_text_color(s_clk_small[gi], sub, 0);
            }
        }
    }
    if (s_page_title) lv_obj_set_style_text_color(s_page_title, val, 0);
    for (int pg = 0; pg < PAGE_COUNT; pg++)
        for (int i = 0; i < ROWS(pg); i++) {
            if (KIND(pg, i) == LAYOUT_GAUGE && s_gz_val[pg][i]) {
                lv_obj_set_style_text_color(s_gz_val[pg][i], val, 0);
                lv_obj_set_style_text_color(s_gz_unit[pg][i], sub, 0);
                lv_obj_set_style_text_color(s_gz_name[pg][i], sub, 0);
                lv_obj_set_style_arc_color(s_gz_arc[pg][i], sub, LV_PART_MAIN);
            }
            if (KIND(pg, i) == LAYOUT_GLANCE) {
                if (s_gl_name[pg][i]) lv_obj_set_style_text_color(s_gl_name[pg][i], sub, 0);
                if (s_gl_state[pg][i]) lv_obj_set_style_text_color(s_gl_state[pg][i], val, 0);
            }
        }
    if (s_status_bg) lv_obj_set_style_bg_color(s_status_bg, lv_color_hex(light ? 0xEEF1F4 : 0x14181C), 0);
    if (s_title_icon) lv_obj_set_style_img_recolor(s_title_icon, val, 0);
    /* weather cards: text colours and the icon (its shapes are coloured per theme) */
    for (int pg = 0; pg < PAGE_COUNT; pg++)
        for (int i = 0; i < ROWS(pg); i++) {
            if (KIND(pg, i) != LAYOUT_WEATHER || !s_wx_temp[pg][i]) continue;
            lv_obj_t *l[] = { s_wx_cond[pg][i], s_wx_temp[pg][i], s_wx_name[pg][i], s_wx_unit[pg][i], s_wx_hum[pg][i] };
            for (int k = 0; k < 5; k++) lv_obj_set_style_text_color(l[k], k < 2 ? val : sub, 0);   /* condition, temperature */
            if (s_wx_last[pg][i][0]) wx_draw_icon(s_wx_icon[pg][i], lv_obj_get_width(s_wx_icon[pg][i]), s_wx_last[pg][i]);
            wx_draw_forecast(pg, i);
        }
    for (int pg = 0; pg < PAGE_COUNT; pg++) {
        if (!s_dot[pg]) continue;
        lv_obj_set_style_bg_color(s_dot[pg], pg == s_page ? fg : lv_color_hex(light ? 0xAEB4BE : 0x4A5058), 0);
    }
}

static void strip_anim_x(void *obj, int32_t x) { lv_obj_set_x(obj, x); }

static void value_select(int g);
/* The shown page, saved once it has stayed for a few seconds (no flash write per swipe) */
static void page_save_cb(lv_timer_t *t)
{
    lv_timer_pause(t);
    nvs_handle_t nv;
    uint8_t old = 0xFF;
    if (nvs_open("ui", NVS_READWRITE, &nv) != ESP_OK) return;
    if (nvs_get_u8(nv, "page", &old) != ESP_OK || old != s_page) {
        nvs_set_u8(nv, "page", (uint8_t)s_page);
        nvs_commit(nv);
    }
    nvs_close(nv);
    /* to HA too, but not every slide of presentation mode */
    if (!s_pres_on && s_page != s_ha_page) {
        s_ha_page = s_page;
        ha_client_set_nav(s_dash_path, s_view_path, s_page);
    }
}

static void show_page(int page)
{
    value_select(-1);
    if (!PAGE_COUNT || !s_strip) return;
    if (page < 0) page = 0;                       /* no wrap-around: stop at either end */
    if (page >= PAGE_COUNT) page = PAGE_COUNT - 1;
    s_page = page;
    title_update();
    static lv_timer_t *save;
    if (!save) save = lv_timer_create(page_save_cb, 5000, NULL);
    lv_timer_reset(save);
    lv_timer_resume(save);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_strip);
    lv_anim_set_exec_cb(&a, strip_anim_x);
    lv_anim_set_values(&a, lv_obj_get_x(s_strip), -s_page * PAGE_W);
    lv_anim_set_time(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    apply_temp_theme(s_light);
    pic_report();
}

/* Network task: new value for one sensor */
static void ha_sensor_changed(int page, int row, const char *value, const char *unit, const char *name)
{
    if (!lvgl_port_lock(0)) return;
    if (page >= 0 && page < PAGE_COUNT && row >= 0 && row < ROWS(page) && KIND(page, row) == LAYOUT_GAUGE && s_gz_val[page][row])
        gauge_update(page, row, value, unit, name);
    if (page >= 0 && page < PAGE_COUNT && row >= 0 && row < ROWS(page) && KIND(page, row) == LAYOUT_GLANCE)
        glance_update(page, row, value, unit, name);
    if (page >= 0 && page < PAGE_COUNT && row >= 0 && row < ROWS(page) && KIND(page, row) == LAYOUT_VALUES && s_val[page][row]) {
        lv_label_set_text(s_val[page][row], value);
        lv_label_set_text(s_unit[page][row], unit);
        {   /* the whole card width: the value is never cut for the icon's sake */
            lv_area_t b;
            graph_block(GI(page, row), &b);
            fit_value(s_val[page][row], s_unit[page][row], lv_area_get_width(&b) - 8);
        }
        if (strncmp(s_full[page][row], name, 31)) {
            snprintf(s_full[page][row], sizeof(s_full[page][row]), "%s", name);
            page_names_update(page);
        }
        place_value(page, row);
    }
    lvgl_port_unlock();
}

/* Network task: graph page value, or a fresh set of history points */
static void graph_value_pos(lv_coord_t *cx, lv_coord_t *y);
static void ha_graph_changed(int graph, int row, const char *latest, const char *unit, const char *name,
                             const float *pts, int n, int64_t end, int hours, bool zero_based)
{
    if (!lvgl_port_lock(0)) return;
    if (graph < 0 || graph >= PAGE_COUNT || row < 0 || row >= ROWS(graph) || KIND(graph, row) != LAYOUT_GRAPH) {
        lvgl_port_unlock();
        return;
    }
    s_gi = GI(graph, row);
    if (!s_chart) { lvgl_port_unlock(); return; }
    (void)hours; (void)end; (void)zero_based;
    if (name && strncmp(s_full[graph][row], name, 31)) {
        snprintf(s_full[graph][row], sizeof(s_full[graph][row]), "%s", name);
        page_names_update(graph);
        s_gi = GI(graph, row);
    }
    if (latest) {
        char v[24];                       /* portrait: whole numbers only (rounded) */
        char *e;
        double x = strtod(latest, &e);
        if (!s_landscape && e != latest && !*e && strchr(latest, '.')) {
            snprintf(v, sizeof(v), "%.0f", x);
            if (!strcmp(v, "-0")) strcpy(v, "0");
            latest = v;
        }
        lv_label_set_text(s_g_val, latest);
        lv_label_set_text(s_g_unit, unit ? unit : "");
        fit_value(s_g_val, s_g_unit, name_max_w(graph, row));   /* clear of the icon */
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
        /* floor: the card's limits.min, else the smallest value shown; top: the peak
         * rounded up to a nice step (about a quarter of the range), with a little headroom */
        float cmin = s_layout.pages[graph].items[row].min;
        float bot = isnan(cmin) ? mn : cmin;
        float step = nice_ceil(LV_MAX(mx - bot, 0.4f) / 4);
        float top = ceilf(mx / step) * step;
        if (top - mx < step * 0.1f) top += step;
        if (top <= bot) top = bot + step;
        lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, (lv_coord_t)(bot * 10), (lv_coord_t)(top * 10));
        /* resample the n smoothed points onto the chart's (fewer) points */
        int cnt = lv_chart_get_point_count(s_chart);
        n = LV_MIN(n, HA_GRAPH_POINTS);
        for (int c = 0; c < cnt; c++) {
            float x = cnt > 1 ? (float)c * (n - 1) / (cnt - 1) : 0;
            int i0 = (int)x, i1 = LV_MIN(i0 + 1, n - 1);
            float f = x - i0, v;
            if (isnan(sm[i0])) v = sm[i1];
            else if (isnan(sm[i1])) v = sm[i0];
            else v = sm[i0] + (sm[i1] - sm[i0]) * f;
            lv_chart_set_value_by_id(s_chart, s_chart_ser, c, isnan(v) ? LV_CHART_POINT_NONE : (lv_coord_t)(v * 10));
        }
        lv_chart_refresh(s_chart);
    }
    lvgl_port_unlock();
}

static float nice_ceil(float x)
{
    if (x <= 0) return 1;
    float mag = powf(10, floorf(log10f(x)));
    static const float steps[] = {1, 2, 2.5f, 5, 10};
    for (int i = 0; i < 5; i++) if (steps[i] * mag >= x) return steps[i] * mag;
    return 10 * mag;
}

/* Area of graph gi on its page: graphs stacked in portrait, side by side in landscape */
static void graph_block(int gi, lv_area_t *a)
{
    int pg = GPG(gi), r = GROW(gi), n = LV_MAX(ROWS(pg), 1);
    lv_coord_t top = TOP_M, bottom = SCR_H - BOTTOM_M;
    if (s_landscape) {
        lv_coord_t w = SCR_W / n;
        a->x1 = r * w; a->x2 = a->x1 + w - 1; a->y1 = top; a->y2 = bottom - 1;
    } else {
        lv_coord_t h = (bottom - top) / n;
        a->x1 = 0; a->x2 = SCR_W - 1; a->y1 = top + r * h; a->y2 = a->y1 + h - 1;
    }
}

/* Each graph block: name at the top, value below it, chart under both */
static void graph_name_pos(lv_coord_t *cx, lv_coord_t *y)
{
    lv_area_t b;
    graph_block(s_gi, &b);
    *cx = b.x1 + GRAPH_X;
    *y = b.y1 + 2;
}

static void graph_value_pos(lv_coord_t *cx, lv_coord_t *y)
{
    graph_name_pos(cx, y);
    *y += ((const lv_font_t *)NFONT)->line_height + 2;
}

static void place_graph_value(void)
{
    lv_coord_t cx, y;
    int g = s_gi;
    bool crisp = s_crisp[g];
    crisp_off(g);
    graph_value_pos(&cx, &y);
    place_pair(s_g_val, s_g_unit, cx, y, true);
    if (s_g_name) page_relayout(GPG(s_gi));
    if (crisp && s_g_name) crisp_on(g);
}

/* Fills the area under each line segment down to the chart's bottom edge, in the line's
 * colour at low opacity; the chart background stays transparent. */
static void chart_fill_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(e);
    if (d->part != LV_PART_ITEMS || d->type != LV_CHART_DRAW_PART_LINE_AND_POINT || !d->p1 || !d->p2) return;
    lv_obj_t *chart = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(chart, &a);
    lv_coord_t bottom = a.y2 - lv_obj_get_style_pad_bottom(chart, 0);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = d->line_dsc ? d->line_dsc->color : lv_color_hex(0x18BCF2);
    r.bg_opa = s_light ? LV_OPA_20 : 41;   /* about 20 % / 16 %, like HA's sensor card */
    r.border_width = 0;
    r.radius = 0;
    /* one column per pixel from p1 up to (not including) p2: neighbouring segments never
     * overlap, so no darker seams where antialiased polygon edges met */
    lv_coord_t x1 = d->p1->x, x2 = d->p2->x, y1 = d->p1->y, y2 = d->p2->y;
    if (x2 <= x1) return;
    for (lv_coord_t x = x1; x < x2; x++) {
        lv_coord_t y = y1 + (lv_coord_t)((int32_t)(y2 - y1) * (x - x1) / (x2 - x1));
        if (y >= bottom) continue;
        lv_area_t col = { x, y, x, bottom };
        lv_draw_rect(d->draw_ctx, &r, &col);
    }
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

static void create_graph(lv_obj_t *page)
{
    /* name on top, value (unit beside it) below, chart filling the rest of the block */
    lv_coord_t cx, y, ny;
    graph_value_pos(&cx, &y);
    graph_name_pos(&cx, &ny);
    s_g_val = lv_label_create(page);
    lv_label_set_text(s_g_val, "--");
    lv_obj_set_style_text_font(s_g_val, VFONT, 0);
    s_g_unit = lv_label_create(page);
    lv_label_set_text(s_g_unit, "");
    lv_obj_set_style_text_font(s_g_unit, UFONT, 0);
    lv_obj_set_style_text_opa(s_g_unit, LV_OPA_COVER, 0);
    place_graph_value();
    s_g_name = lv_label_create(page);
    const layout_item_t *it = &s_layout.pages[GPG(s_gi)].items[GROW(s_gi)];
    lv_obj_set_style_text_font(s_g_name, NFONT, 0);
    snprintf(s_full[GPG(s_gi)][GROW(s_gi)], 32, "%.31s", it->name[0] ? it->name : it->entity);
    (void)ny;
    if (icon_slot(GPG(s_gi), GROW(s_gi))) {
        icon_create(page, GPG(s_gi), GROW(s_gi), icon_color(), GRAPH_ICON);
        icon_place(GPG(s_gi), GROW(s_gi));
    }

    /* line chart below the value to the bottom of the block, no axes or grid */
    lv_area_t b;
    graph_block(s_gi, &b);
    s_chart = lv_chart_create(page);
    lv_coord_t top = y + ((const lv_font_t *)VFONT)->line_height + 2;
    lv_obj_set_size(s_chart, lv_area_get_width(&b), LV_MAX(b.y2 + 1 - top, 10));
    lv_obj_set_pos(s_chart, b.x1, top);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_radius(s_chart, 0, 0);
    lv_obj_set_style_pad_all(s_chart, 2, 0);
    lv_chart_set_div_line_count(s_chart, 0, 0);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    /* fewer points than pixels: at one point per pixel LVGL draws plain vertical strokes,
     * without antialiasing or the fill underneath */
    lv_chart_set_point_count(s_chart, LV_MIN(HA_GRAPH_POINTS, (lv_obj_get_style_width(s_chart, 0) - 4) / 2));
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 10);
    lv_obj_set_style_line_width(s_chart, HA_GRAPH_LINE_WIDTH, LV_PART_ITEMS);
    lv_obj_set_style_line_rounded(s_chart, true, LV_PART_ITEMS);   /* round joints */
    lv_obj_set_style_size(s_chart, 0, LV_PART_INDICATOR);          /* no point markers */
    s_chart_ser = lv_chart_add_series(s_chart, lv_color_hex(0x18BCF2), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_add_event_cb(s_chart, chart_fill_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_chart_set_all_value(s_chart, s_chart_ser, LV_CHART_POINT_NONE);
}


static void create_pages(lv_obj_t *scr)
{
    if (!PAGE_COUNT) {   /* nothing from HA or the cache yet */
        lv_obj_t *l = lv_label_create(scr);
        lv_label_set_text(l, "Loading layout...");
        lv_obj_set_style_text_font(l, &lv_font_latin_16, 0);
        lv_obj_set_style_text_color(l, s_light ? lv_color_hex(0x1C1C1E) : lv_color_white(), 0);
        lv_obj_center(l);
        s_strip = NULL;
        s_page_title = NULL;
        return;
    }
    s_strip = lv_obj_create(scr);
    lv_obj_remove_style_all(s_strip);
    lv_obj_set_size(s_strip, PAGE_W * PAGE_COUNT, SCR_H);
    lv_obj_set_pos(s_strip, 0, 0);
    lv_obj_clear_flag(s_strip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    for (int pg = 0; pg < PAGE_COUNT; pg++) {
        lv_obj_t *page = make_page(pg);
        for (int i = 0; i < ROWS(pg); i++) {   /* one block per card, any mix of kinds */
            const layout_item_t *it = &s_layout.pages[pg].items[i];
            if (it->kind == LAYOUT_GRAPH) { s_gi = GI(pg, i); create_graph(page); continue; }
            if (it->kind == LAYOUT_BUTTONS) { create_button(page, pg, i); continue; }
            if (it->kind == LAYOUT_CLOCK) { create_clock(page, pg, i); continue; }
            if (it->kind == LAYOUT_PICTURE) { create_picture(page, pg, i); continue; }
            if (it->kind == LAYOUT_WEATHER) { create_weather(page, pg, i); continue; }
            if (it->kind == LAYOUT_GAUGE) { create_gauge(page, pg, i); continue; }
            if (it->kind == LAYOUT_GLANCE) { create_glance(page, pg, i); continue; }
            lv_coord_t cx, y;
            value_pos(pg, i, &cx, &y);
            s_name[pg][i] = lv_label_create(page);
            lv_obj_set_style_text_font(s_name[pg][i], NFONT, 0);
            snprintf(s_full[pg][i], sizeof(s_full[pg][i]), "%.31s", it->name[0] ? it->name : it->entity);
            (void)cx; (void)y;
            /* value centred on the page; unit placed right next to it */
            s_val[pg][i] = lv_label_create(page);
            lv_label_set_text(s_val[pg][i], "--");
            lv_obj_set_style_text_font(s_val[pg][i], VFONT, 0);
            s_unit[pg][i] = lv_label_create(page);
            lv_label_set_text(s_unit[pg][i], "");
            lv_obj_set_style_text_font(s_unit[pg][i], UFONT, 0);
            lv_obj_set_style_text_opa(s_unit[pg][i], LV_OPA_COVER, 0);
            place_value(pg, i);
            if (icon_slot(pg, i)) { icon_create(page, pg, i, icon_color(), ICON_SIZE); icon_place(pg, i); }
        }
        page_names_update(pg);
    }
    s_gi = 0;

    /* Page dots, centred at the bottom (no background); fixed on the screen, not part of the
     * strip, above the pages (which run under them) */
    const lv_coord_t d = 6, gap = 10;   /* dot 20 % smaller than the original 8 px */
    lv_coord_t total = PAGE_COUNT * d + (PAGE_COUNT - 1) * gap;
    s_dots_box = lv_obj_create(scr);
    lv_obj_remove_style_all(s_dots_box);
    lv_obj_clear_flag(s_dots_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_dots_box, total + 16, d + 10);
    lv_obj_set_style_radius(s_dots_box, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(s_dots_box, LV_ALIGN_BOTTOM_MID, 0, -(DOTS_H - d - 10) / 2);
    for (int pg = 0; pg < PAGE_COUNT; pg++) {
        s_dot[pg] = lv_obj_create(s_dots_box);
        lv_obj_remove_style_all(s_dot[pg]);
        lv_obj_set_size(s_dot[pg], d, d);
        lv_obj_set_style_radius(s_dot[pg], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_dot[pg], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_dot[pg], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(s_dot[pg], 8 + pg * (d + gap), 5);
    }
    dots_show();

    s_status_bg = lv_obj_create(scr);     /* above the pages, below title and status icons */
    lv_obj_remove_style_all(s_status_bg);
    lv_obj_clear_flag(s_status_bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_status_bg, SCR_W, STATUS_H + 4);
    lv_obj_set_style_bg_opa(s_status_bg, LV_OPA_40, 0);   /* 60 % transparent */
    lv_obj_set_style_bg_color(s_status_bg, lv_color_hex(s_light ? 0xEEF1F4 : 0x14181C), 0);
    s_page_title = lv_label_create(scr);
    lv_obj_set_style_text_font(s_page_title, &lv_font_latin_16, 0);
    lv_label_set_long_mode(s_page_title, LV_LABEL_LONG_DOT);   /* ends in "..." when too long */
    lv_obj_set_size(s_page_title, SCR_W - 10 - 52, lv_font_latin_16.line_height);   /* one line, short of the status icons */
    /* vertically centred on the status icons (HA dot centre is at y = 14 + 10/2) */
    s_title_icon = lv_img_create(scr);
    lv_obj_clear_flag(s_title_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_img_recolor_opa(s_title_icon, LV_OPA_COVER, 0);
    /* centred on y = 19, like the Wi-Fi symbol and HA dot on the right */
    lv_obj_set_pos(s_title_icon, 8, 19 - TITLE_ICON / 2);
    title_update();
    apply_temp_theme(s_light);
}


/* ---- Tapped value grows (only the number; unit and name keep their size and move along) ----------- */
#define GROUPS     (LAYOUT_MAX_PAGES * LAYOUT_MAX_ROWS)   /* g = page * LAYOUT_MAX_ROWS + row */
#define ZOOM_BIG   ((uint16_t)(HA_ACTIVE_SCALE * 256 + 0.5f))   /* LVGL zoom: 256 = 100 % */
static int s_sel = -1;
static uint16_t s_zoom[GROUPS];
/* A grown value is drawn in a real font of its size once the animation ends (LVGL's scaling
 * of a rendered label looks washed out); s_base = its area in the normal font */
static lv_area_t s_base[GROUPS];
static const lv_font_t *s_base_font[GROUPS];   /* the normal font while s_crisp */
/* the clock font nearest to 1.6 x the normal one (digits and signs only) */
static const lv_font_t *big_font(const lv_font_t *base)
{
    static const lv_font_t *const F[] = { &lv_font_clock_24, &lv_font_clock_28, &lv_font_clock_32, &lv_font_clock_36,
        &lv_font_clock_40, &lv_font_clock_44, &lv_font_clock_48, &lv_font_clock_56, &lv_font_clock_64, &lv_font_clock_72 };
    int want = base->line_height * 16 / 10, best = 0;
    for (int k = 1; k < (int)(sizeof(F) / sizeof(F[0])); k++)
        if (LV_ABS(F[k]->line_height - want) < LV_ABS(F[best]->line_height - want)) best = k;
    return F[best];
}

/* o[0] = NULL when group g doesn't exist in the current layout */
static void group_objs(int g, lv_obj_t *o[3])
{
    int pg = g / LAYOUT_MAX_ROWS, row = g % LAYOUT_MAX_ROWS;
    o[0] = o[1] = o[2] = NULL;
    if (pg >= PAGE_COUNT || row >= ROWS(pg)) return;
    if (KIND(pg, row) == LAYOUT_GRAPH) {
        int gi = GI(pg, row);   /* index directly: s_gi belongs to the caller */
        o[0] = s_g_val_[gi]; o[1] = s_g_unit_[gi]; o[2] = s_g_name_[gi];
        return;
    }
    if (KIND(pg, row) == LAYOUT_GAUGE) {
        o[0] = s_gz_val[pg][row]; o[1] = s_gz_unit[pg][row]; o[2] = s_gz_name[pg][row];
        return;
    }
    if (KIND(pg, row) != LAYOUT_VALUES) return;   /* buttons and clocks don't grow */
    o[0] = s_val[pg][row]; o[1] = s_unit[pg][row]; o[2] = s_name[pg][row];
}

static int group_page(int g)
{
    return g / LAYOUT_MAX_ROWS;
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

/* Tap area of a group: its labels; for a graph page also the chart, i.e. the whole block */
static void group_area(int g, lv_area_t *a)
{
    group_area_n(g, 3, a);
    int pg = group_page(g);
    if (pg < PAGE_COUNT && GROW(g) < ROWS(pg) && KIND(pg, GROW(g)) == LAYOUT_GRAPH && s_chart_[g]) {   /* g == GI(page, row) */
        lv_area_t c;
        lv_obj_get_coords(s_chart_[g], &c);
        a->x1 = LV_MIN(a->x1, c.x1);
        a->y1 = LV_MIN(a->y1, c.y1);
        a->x2 = LV_MAX(a->x2, c.x2);
        a->y2 = LV_MAX(a->y2, c.y2);
    }
}

/* Lay out every element on page pg for the current zoom levels. The active number grows
 * about its centre; its unit keeps its size and follows the number's right edge and
 * baseline; its name moves down to keep the gap. Neighbours move apart by the extra size
 * (rows up/down in portrait, columns left/right in landscape), and an element that would
 * cross the screen (or graph) edge is pushed back inside, so nothing gets cut off. */
static void page_relayout(int pg)
{
    int gs[LAYOUT_MAX_ROWS], n = 0;
    lv_obj_t *probe[3];
    for (int g = pg * LAYOUT_MAX_ROWS; g < (pg + 1) * LAYOUT_MAX_ROWS && g < GROUPS; g++) {
        group_objs(g, probe);
        if (probe[0]) gs[n++] = g;
    }
    lv_area_t va[LAYOUT_MAX_ROWS], ua[LAYOUT_MAX_ROWS], na[LAYOUT_MAX_ROWS];
    float f[LAYOUT_MAX_ROWS];
    lv_obj_t *o[LAYOUT_MAX_ROWS][3];
    for (int k = 0; k < n; k++) {
        group_objs(gs[k], o[k]);
        if (!o[k][0]) return;
        f[k] = ((s_zoom[gs[k]] ? s_zoom[gs[k]] : 256) - 256) / 256.0f;
        lv_area_t *ar[3] = {&va[k], &ua[k], &na[k]};
        for (int i = 0; i < 3; i++) {       /* untranslated positions */
            lv_obj_get_coords(o[k][i], ar[i]);
            lv_area_move(ar[i], -lv_obj_get_style_translate_x(o[k][i], 0), -lv_obj_get_style_translate_y(o[k][i], 0));
        }
        if (s_crisp[gs[k]]) va[k] = s_base[gs[k]];   /* lay out as the scaled normal-font value */
    }
    /* a page of only tiles keeps the old behaviour (rows/columns make room for a grown value);
     * on mixed pages every value stays inside its own block */
    bool pure = true;
    for (int i = 0; i < ROWS(pg); i++) pure &= KIND(pg, i) == LAYOUT_VALUES;
    bool cols = s_landscape && pure;
    lv_coord_t right_bound = SCR_W - 2;
    const lv_font_t *vf;
    for (int k = 0; k < n; k++) {
        /* push away from the neighbours' growth */
        float shift = 0;
        for (int j = 0; j < n && pure; j++) {
            if (j == k) continue;
            float ext = cols ? lv_area_get_width(&va[j]) * f[j] : lv_area_get_height(&va[j]) * f[j];
            shift += (j < k ? ext : -ext) / 2;
        }
        lv_coord_t tx = cols ? (lv_coord_t)shift : 0, ty = cols ? 0 : (lv_coord_t)shift;
        lv_coord_t w = lv_area_get_width(&va[k]), h = lv_area_get_height(&va[k]);
        lv_coord_t gw = (lv_coord_t)(w * f[k]), gh = (lv_coord_t)(h * f[k]);
        /* graphs: the value grows from its top-left corner (away from the name above it and
         * the status bar); rows: about its centre */
        bool corner = KIND(pg, GROW(gs[k])) == LAYOUT_GRAPH;
        lv_coord_t udx = corner ? gw : gw / 2;
        vf = s_crisp[gs[k]] ? s_base_font[gs[k]] : lv_obj_get_style_text_font(o[k][0], 0);   /* gauges: own size */
        lv_coord_t udy = (lv_coord_t)(((vf->line_height - vf->base_line) - (corner ? 0 : h / 2)) * f[k]);
        /* keep the whole element on screen */
        /* page-relative: the page may be anywhere on the strip (even off screen) */
        lv_area_t pa;
        lv_obj_get_coords(lv_obj_get_parent(o[k][0]), &pa);
        lv_coord_t left = LV_MIN(va[k].x1 - (corner ? 0 : gw / 2), na[k].x1) + tx - pa.x1;
        lv_coord_t right = LV_MAX(LV_MAX(va[k].x2 + (corner ? gw : gw / 2), ua[k].x2 + udx), na[k].x2) + tx - pa.x1;
        lv_coord_t lo = 2, hi = right_bound;
        if (!pure) {                      /* stay inside the card's block */
            lv_area_t blk;
            graph_block(gs[k], &blk);
            lo = blk.x1 + 2;
            hi = blk.x2 - 2;
        }
        if (right > hi) tx -= right - hi;
        else if (left < lo) tx += lo - left;
        lv_obj_set_style_transform_pivot_x(o[k][0], corner ? 0 : w / 2, 0);
        lv_obj_set_style_transform_pivot_y(o[k][0], corner ? 0 : h / 2, 0);
        if (s_crisp[gs[k]]) {
            /* big font, no scaling: put it where the scaled value would be (same baseline,
             * left edge for graphs, centre for tiles) */
            const lv_font_t *bf = lv_obj_get_style_text_font(o[k][0], 0);
            float z = 1 + f[k];
            lv_coord_t px = corner ? 0 : w / 2, py = corner ? 0 : h / 2;
            lv_coord_t sx1 = va[k].x1 + px - (lv_coord_t)(px * z), sy1 = va[k].y1 + py - (lv_coord_t)(py * z);
            lv_area_t ra;
            lv_obj_get_coords(o[k][0], &ra);
            lv_area_move(&ra, -lv_obj_get_style_translate_x(o[k][0], 0), -lv_obj_get_style_translate_y(o[k][0], 0));
            lv_coord_t want_x1 = corner ? sx1 : sx1 + ((lv_coord_t)(w * z) - lv_area_get_width(&ra)) / 2;
            lv_coord_t base_scaled = sy1 + (lv_coord_t)((vf->line_height - vf->base_line) * z);
            lv_coord_t base_real = ra.y1 + (bf->line_height - bf->base_line);
            lv_obj_set_style_transform_zoom(o[k][0], 256, 0);
            lv_obj_set_style_translate_x(o[k][0], want_x1 - ra.x1 + tx, 0);
            lv_obj_set_style_translate_y(o[k][0], base_scaled - base_real + ty, 0);
        } else {
            lv_obj_set_style_transform_zoom(o[k][0], (lv_coord_t)(256 + f[k] * 256 + 0.5f), 0);
            lv_obj_set_style_translate_x(o[k][0], tx, 0);
            lv_obj_set_style_translate_y(o[k][0], ty, 0);
        }
        lv_obj_set_style_translate_x(o[k][1], tx + udx, 0);
        lv_obj_set_style_translate_y(o[k][1], ty + udy, 0);
        lv_obj_set_style_translate_x(o[k][2], tx, 0);
        /* the name below a row moves down with the growing value; the graph name above stays */
        lv_obj_set_style_translate_y(o[k][2], corner ? 0 : ty + gh / 2, 0);
        /* a tile's icon moves with the growing value: out to its left (portrait) or up
         * above it (landscape) */
        int pg_k = group_page(gs[k]), row_k = gs[k] % LAYOUT_MAX_ROWS;
        lv_obj_t *ic = !corner ? s_icon[pg_k][row_k] : NULL;
        if (ic) {
            lv_obj_set_style_translate_x(ic, s_landscape ? tx : tx - gw / 2, 0);
            lv_obj_set_style_translate_y(ic, s_landscape ? ty - gh / 2 : ty, 0);
        }
    }
}

/* only number-like text: the big fonts are digits and signs */
static bool crisp_ok(const char *t)
{
    if (!t[0]) return false;
    for (; *t; t++) if (!strchr("0123456789.,-+%: ", *t)) return false;
    return true;
}

static void crisp_off(int g)
{
    if (g < 0 || !s_crisp[g]) return;
    lv_obj_t *o[3];
    group_objs(g, o);
    s_crisp[g] = false;
    if (o[0]) { lv_obj_set_style_text_font(o[0], s_base_font[g] ? s_base_font[g] : VFONT, 0); lv_obj_update_layout(o[0]); }
}

static void crisp_on(int g)
{
    lv_obj_t *o[3];
    group_objs(g, o);
    if (!o[0] || s_crisp[g] || !crisp_ok(lv_label_get_text(o[0]))) return;
    lv_obj_update_layout(o[0]);
    lv_obj_get_coords(o[0], &s_base[g]);   /* normal font, untranslated */
    lv_area_move(&s_base[g], -lv_obj_get_style_translate_x(o[0], 0), -lv_obj_get_style_translate_y(o[0], 0));
    s_base_font[g] = lv_obj_get_style_text_font(o[0], 0);
    lv_obj_set_style_text_font(o[0], big_font(s_base_font[g]), 0);
    lv_obj_update_layout(o[0]);
    s_crisp[g] = true;
    page_relayout(group_page(g));
}

static void group_zoom_cb(void *var, int32_t z)
{
    int g = (int)((uint16_t *)var - s_zoom);
    s_zoom[g] = (uint16_t)z;
    page_relayout(group_page(g));
    if (z == ZOOM_BIG && g == s_sel) crisp_on(g);   /* animation done: sharp text */
}

static void group_animate(int g, uint16_t to)
{
    crisp_off(g);                         /* animate the normal font */
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
    int old = s_sel;
    s_sel = g;                            /* first: an animation applies its start value at once */
    if (old >= 0) group_animate(old, 256);
    if (g >= 0) group_animate(g, ZOOM_BIG);
}

static void value_tap(lv_point_t p);
/* screenshot tool: a tap without a finger */

/* A tap on the main screen: select the value under the finger, or none */
static void value_tap(lv_point_t p)
{
    if (button_tap(p)) return;
    for (int g = 0; g < GROUPS; g++) {
        if (group_page(g) != s_page) continue;
        lv_obj_t *o[3];
        group_objs(g, o);
        if (!o[0]) continue;
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
    lv_obj_set_style_text_font(label, &lv_font_latin_16, 0);
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
#define CFG_PAGES    4    /* settings (refresh, view), display, presentation, errors */
#define CFG_PRES_PAGE 2
#define CFG_ERR_PAGE 3
static const char *const s_cfg_titles[CFG_PAGES] = {"Settings", "Display", "Presentation", "Errors"};

/* ---- Presentation mode: cycle through the pages every s_pres_secs, wrapping at the end.
 * A touch pauses it (PRES_PAUSE_MS after the last touch, or one interval if longer), and it
 * never advances while the config panel is open. On/off and the interval are kept in NVS. */
#define PRES_PAUSE_MS 30000
static const uint16_t PRES_SECS[] = {5, 10, 15, 30, 60, 120, 300};
static uint32_t s_pres_last;          /* tick of the last advance or touch */
static lv_timer_t *s_pres_timer;
static lv_obj_t *s_pres_btn, *s_pres_int_btn, *s_pres_caption;
static bool s_cfg_open, s_paging, s_dragging;   /* defined below */

static bool s_pres_touched;          /* the last reset was a touch: wait at least PRES_PAUSE_MS */
static void pres_touch(void) { s_pres_last = lv_tick_get(); s_pres_touched = true; }

static void pres_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_pres_on || s_cfg_open || PAGE_COUNT < 2 || s_paging || s_dragging) return;
    uint32_t wait = (uint32_t)s_pres_secs * 1000;
    if (s_pres_touched && wait < PRES_PAUSE_MS) wait = PRES_PAUSE_MS;
    if (lv_tick_elaps(s_pres_last) < wait) return;
    s_pres_last = lv_tick_get();
    s_pres_touched = false;
    show_page((s_page + 1) % PAGE_COUNT);
}

static void pres_save(void)
{
    nvs_handle_t nv;
    if (nvs_open("ui", NVS_READWRITE, &nv) != ESP_OK) return;
    nvs_set_u8(nv, "pres_on", s_pres_on);
    nvs_set_u16(nv, "pres_s", s_pres_secs);
    nvs_commit(nv);
    nvs_close(nv);
}

/* A local change: kept in NVS and sent to HA; polls under way don't undo it */
static void pres_changed(void)
{
    pres_save();
    s_pres_pending = true;
    ha_client_set_presentation(s_pres_on, s_pres_secs);
}

static void pres_labels(void)
{
    if (s_pres_btn) lv_label_set_text(lv_obj_get_child(s_pres_btn, 0), s_pres_on ? "Stop" : "Start");
    if (s_pres_int_btn) {
        char b[24];
        if (s_pres_secs >= 60) snprintf(b, sizeof(b), "Every %u min", s_pres_secs / 60);
        else snprintf(b, sizeof(b), "Every %u s", s_pres_secs);
        lv_label_set_text(lv_obj_get_child(s_pres_int_btn, 0), b);
    }
}

static void pres_toggle_cb(lv_event_t *e)
{
    (void)e;
    if (s_swiped) return;
    s_pres_on = !s_pres_on;
    pres_touch();
    pres_labels();
    pres_changed();
}

static void pres_interval_cb(lv_event_t *e)
{
    (void)e;
    if (s_swiped) return;
    int n = sizeof(PRES_SECS) / sizeof(PRES_SECS[0]), i = 0;
    while (i < n && PRES_SECS[i] <= s_pres_secs) i++;   /* next step up (HA may hold any value) */
    s_pres_secs = PRES_SECS[i % n];
    pres_touch();
    pres_labels();
    pres_changed();
}

/* HA's values (LVGL task, via lv_async_call): packed as secs << 1 | on */
static void pres_apply(void *arg)
{
    int v = (int)(intptr_t)arg;
    bool on = v & 1;
    int secs = v >> 1;
    if (s_pres_pending) return;
    if (secs < 1) secs = s_pres_secs;
    if (on == s_pres_on && secs == s_pres_secs) return;
    if (on && !s_pres_on) pres_touch();
    s_pres_on = on;
    s_pres_secs = secs > 3600 ? 3600 : secs;
    pres_labels();
    pres_save();
}

/* Network task: presentation entities' state */
static void ha_pres_changed(bool on, int secs)
{
    if (!lvgl_port_lock(0)) return;
    if (!s_pres_pending) lv_async_call(pres_apply, (void *)(intptr_t)((secs << 1) | on));
    lvgl_port_unlock();
}
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
    if (page < 0) page = 0;
    if (page >= CFG_PAGES) page = CFG_PAGES - 1;
    s_cfg_page = page;
    lv_label_set_text(s_cfg_title, s_cfg_titles[page]);
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
    if (!open) view_menu_close();
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

/* Serial 'C<n>': open the panel on page n; n >= CFG_PAGES closes it */
static void cfg_test_open(int page)
{
    if (page >= CFG_PAGES) { cfg_slide_to(false); return; }
    cfg_show_page(page);
    cfg_slide_to(true);
}

/* Registered on the main screen (to open) and on the panel background (to close) */
static void edge_swipe_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);

    if (code == LV_EVENT_PRESSED) {
        dots_show();
        pres_touch();
        s_dragging = s_cfg_open || p.y <= CFG_EDGE;
        s_paging = !s_dragging && s_strip;
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
                bool can = s_cfg_page == CFG_ERR_PAGE && (dy > 0 ? s_err_scroll > 0 : s_err_scroll < err_scroll_max());
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
    s_page_title = s_title_icon = s_status_bg = NULL;
    s_dots_box = NULL;                    /* deleted with the screen: don't touch them */
    s_wifi_icon[0] = s_ha_icon[0] = s_wifi_icon[1] = s_ha_icon[1] = NULL;
    pic_free_all();
    /* back on the page shown before the reboot, once there are pages */
    if (s_boot_page >= 0 && PAGE_COUNT) { s_page = s_boot_page; s_boot_page = -1; }
    /* stay on the current page across rebuilds (rotation, new layout) when it still exists */
    if (s_page >= PAGE_COUNT) s_page = 0;
    s_sel = -1;
    memset(s_zoom, 0, sizeof(s_zoom));
    memset(s_crisp, 0, sizeof(s_crisp));
    s_cfg_open = false;               /* the panel page is kept across rebuilds */
    s_dragging = s_paging = false;
    memset(s_val, 0, sizeof(s_val));
    memset(s_icon, 0, sizeof(s_icon));
    memset(s_gz_val, 0, sizeof(s_gz_val));
    memset(s_gl_name, 0, sizeof(s_gl_name));
    memset(s_gl_state, 0, sizeof(s_gl_state));
    icons_free();
    memset(s_unit, 0, sizeof(s_unit));
    memset(s_name, 0, sizeof(s_name));
    memset(s_g_val_, 0, sizeof(s_g_val_));
    memset(s_g_unit_, 0, sizeof(s_g_unit_));
    memset(s_g_name_, 0, sizeof(s_g_name_));
    memset(s_chart_, 0, sizeof(s_chart_));
    memset(s_btn, 0, sizeof(s_btn));
    memset(s_btn_name, 0, sizeof(s_btn_name));
    memset(s_btn_state, 0, sizeof(s_btn_state));
    memset(s_btn_pending, 0, sizeof(s_btn_pending));
    memset(s_btn_st, 0, sizeof(s_btn_st));   /* "--" until the first poll */
    memset(s_clk_time, 0, sizeof(s_clk_time));
    memset(s_clk_small, 0, sizeof(s_clk_small));
    memset(s_clk_bg, 0, sizeof(s_clk_bg));
    memset(s_wx_temp, 0, sizeof(s_wx_temp));
    memset(s_fc_box, 0, sizeof(s_fc_box));
    for (int p = 0; p < LAYOUT_MAX_PAGES; p++)
        for (int r = 0; r < LAYOUT_MAX_ROWS; r++) { free(s_fc_data[p][r]); s_fc_data[p][r] = NULL; }
    memset(s_full, 0, sizeof(s_full));
    s_err_box = s_err_label = NULL;
    s_view_btn = s_view_caption = s_view_menu = NULL;
    s_pres_btn = s_pres_int_btn = s_pres_caption = NULL;
    s_dash_btn = s_dash_caption = NULL;
    memset(s_cfg_dot, 0, sizeof(s_cfg_dot));
    set_screen_bg(scr, s_light);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, edge_swipe_cb, LV_EVENT_ALL, NULL);

    /* Main screen: sensor pages and status line */
    create_pages(scr);
    if (s_strip) lv_obj_set_x(s_strip, -s_page * PAGE_W);
    pic_report();
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

    /* Panel pages side by side (settings, display, errors), dragged left/right like the main screen */
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
    lv_obj_set_x(s_cfg_strip, -s_cfg_page * CFG_W);

    s_cfg_title = lv_label_create(s_cfg);
    lv_label_set_text(s_cfg_title, s_cfg_titles[s_cfg_page]);
    lv_obj_set_style_text_font(s_cfg_title, &lv_font_montserrat_20, 0);
    lv_obj_align(s_cfg_title, LV_ALIGN_TOP_LEFT, 10, 19 - lv_font_montserrat_20.line_height / 2);

    /* Settings page: dashboard, view, refresh. Portrait: one centred column; landscape:
     * dashboard left, view right, refresh centred below */
    s_dash_caption = lv_label_create(cp[0]);
    lv_label_set_text(s_dash_caption, "Dashboard");
    lv_obj_set_style_text_font(s_dash_caption, &lv_font_latin_14, 0);
    s_dash_btn = make_button(cp[0], "", lv_color_hex(0x5E5E6E));
    lv_obj_t *dl = lv_obj_get_child(s_dash_btn, 0);
    lv_label_set_long_mode(dl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(dl, 148 - 16);
    lv_obj_set_style_text_align(dl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_dash_btn, LV_ALIGN_TOP_MID, s_landscape ? -SCR_W / 4 : 0, s_landscape ? 56 : 60);
    lv_obj_align_to(s_dash_caption, s_dash_btn, LV_ALIGN_OUT_TOP_LEFT, 0, -4);
    lv_obj_add_event_cb(s_dash_btn, dash_cb, LV_EVENT_CLICKED, NULL);
    dash_btn_update();

    s_view_caption = lv_label_create(cp[0]);
    lv_label_set_text(s_view_caption, "View");
    lv_obj_set_style_text_font(s_view_caption, &lv_font_latin_14, 0);
    s_view_btn = make_button(cp[0], "", lv_color_hex(0x5E5E6E));
    lv_obj_t *vl = lv_obj_get_child(s_view_btn, 0);
    lv_label_set_long_mode(vl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(vl, 148 - 16);
    lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_view_btn, LV_ALIGN_TOP_MID, s_landscape ? SCR_W / 4 : 0, s_landscape ? 56 : 132);
    lv_obj_align_to(s_view_caption, s_view_btn, LV_ALIGN_OUT_TOP_LEFT, 0, -4);
    lv_obj_add_event_cb(s_view_btn, view_cb, LV_EVENT_CLICKED, NULL);
    view_btn_update();

    /* Display page. Portrait: one column (theme, orientation, brightness); landscape: theme
     * and orientation left, brightness right */
    const lv_coord_t colx = s_landscape ? SCR_W / 4 : 0;   /* column offset from centre */
    lv_obj_t *btn = make_button(cp[1], "Theme", lv_color_hex(0x5E5E6E));
    lv_obj_align(btn, LV_ALIGN_TOP_MID, -colx, s_landscape ? 40 : 44);
    lv_obj_add_event_cb(btn, ha_toggle_cb, LV_EVENT_CLICKED, NULL);
    s_ha_btn = btn;

    /* Orientation: the label names the action, like the theme button */
    lv_obj_t *ob = make_button(cp[1], s_landscape ? "Portrait" : "Landscape", lv_color_hex(0x5E5E6E));
    lv_obj_align(ob, LV_ALIGN_TOP_MID, -colx, s_landscape ? 96 : 106);
    lv_obj_add_event_cb(ob, orient_cb, LV_EVENT_CLICKED, NULL);
    s_orient_btn = ob;

    s_bri_caption = lv_label_create(cp[1]);
    lv_label_set_text(s_bri_caption, "Brightness");
    lv_obj_set_style_text_font(s_bri_caption, &lv_font_latin_14, 0);
    if (s_landscape) lv_obj_set_pos(s_bri_caption, SCR_W / 2 + (SCR_W / 2 - 148) / 2, 56);
    else lv_obj_align_to(s_bri_caption, ob, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 20);

    lv_obj_t *sl = lv_slider_create(cp[1]);
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

    /* Presentation page: start/stop, then the interval; one centred column */
    s_pres_btn = make_button(cp[CFG_PRES_PAGE], "", lv_color_hex(0x5E5E6E));
    lv_obj_align(s_pres_btn, LV_ALIGN_TOP_MID, s_landscape ? -SCR_W / 4 : 0, s_landscape ? 64 : 44);
    lv_obj_add_event_cb(s_pres_btn, pres_toggle_cb, LV_EVENT_CLICKED, NULL);
    s_pres_caption = lv_label_create(cp[CFG_PRES_PAGE]);
    lv_label_set_text(s_pres_caption, "Next page");
    lv_obj_set_style_text_font(s_pres_caption, &lv_font_latin_14, 0);
    s_pres_int_btn = make_button(cp[CFG_PRES_PAGE], "", lv_color_hex(0x5E5E6E));
    lv_obj_align(s_pres_int_btn, LV_ALIGN_TOP_MID, s_landscape ? SCR_W / 4 : 0, s_landscape ? 64 : 128);
    lv_obj_align_to(s_pres_caption, s_pres_int_btn, LV_ALIGN_OUT_TOP_LEFT, 0, -4);
    lv_obj_add_event_cb(s_pres_int_btn, pres_interval_cb, LV_EVENT_CLICKED, NULL);
    pres_labels();
    if (!s_pres_timer) { s_pres_timer = lv_timer_create(pres_timer_cb, 1000, NULL); pres_touch(); }

    /* Errors page: HA errors from the last poll, scrolls when the text is long */
    lv_coord_t ey = STATUS_H + 6;
    /* A plain, non-clickable clip area: touches pass through to the panel, which scrolls
     * the text itself (see edge_swipe_cb) */
    s_err_box = lv_obj_create(cp[CFG_ERR_PAGE]);
    lv_obj_remove_style_all(s_err_box);
    lv_obj_set_size(s_err_box, CFG_W - 20, SCR_H - ey - DOTS_H - 4);
    lv_obj_set_pos(s_err_box, 10, ey);
    lv_obj_clear_flag(s_err_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_err_label = lv_label_create(s_err_box);
    lv_label_set_long_mode(s_err_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_err_label, lv_pct(100));
    lv_obj_set_style_text_font(s_err_label, &lv_font_latin_14, 0);
    lv_label_set_text(s_err_label, NO_ERRORS);

    /* Page dots at the bottom of the panel */
    for (int i = 0; i < CFG_PAGES; i++) {
        s_cfg_dot[i] = lv_obj_create(s_cfg);
        lv_obj_remove_style_all(s_cfg_dot[i]);
        lv_obj_set_size(s_cfg_dot[i], 6, 6);
        lv_obj_set_style_radius(s_cfg_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_cfg_dot[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_cfg_dot[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(s_cfg_dot[i], LV_ALIGN_BOTTOM_MID, (i * 2 - (CFG_PAGES - 1)) * 8, -(DOTS_H - 6) / 2);
    }

    /* Status icons on the panel too, since the panel covers the main screen's */
    create_status_icons(s_cfg, 1);
    show_status(HA_STATE_OFFLINE);
    apply_panel_theme(s_light);

    /* Refresh: re-read everything from Home Assistant now */
    s_refresh_btn = make_button(cp[0], "Connecting...", lv_color_hex(0x5E5E6E));
    lv_obj_align(s_refresh_btn, LV_ALIGN_TOP_MID, 0, s_landscape ? 106 : 196);
    lv_obj_add_event_cb(s_refresh_btn, refresh_cb, LV_EVENT_CLICKED, NULL);

}

/* Panel colours for the current theme. Must be called with the LVGL lock held. */
static void apply_panel_theme(bool light)
{
    if (!s_cfg) return;
    lv_obj_set_style_bg_color(s_cfg, lv_color_hex(light ? 0xF4F6F9 : 0x15181D), 0);
    lv_color_t text = light ? lv_color_hex(0x1C1C1E) : lv_color_white();
    lv_obj_set_style_text_color(s_cfg_title, text, 0);
    lv_obj_set_style_text_color(s_bri_caption, text, 0);
    if (s_view_caption) lv_obj_set_style_text_color(s_view_caption, text, 0);
    if (s_pres_caption) lv_obj_set_style_text_color(s_pres_caption, text, 0);
    if (s_dash_caption) lv_obj_set_style_text_color(s_dash_caption, text, 0);
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

/* While a refresh or a view change is on its way, the refresh and view buttons take no
 * input. Theme, orientation and brightness stay usable: they apply at once and are sent
 * to Home Assistant in the background. Must be called with the LVGL lock held. */
static void set_busy(bool busy)
{
    s_busy = busy;
    /* disabled: a muted button with text that stays readable (not just faded out) */
    lv_color_t bg = lv_color_hex(s_light ? 0xD3D8DF : 0x33383F);
    lv_color_t fg = lv_color_hex(s_light ? 0x4A5260 : 0xB4BAC4);
    lv_obj_t *objs[] = {s_refresh_btn, s_view_btn, s_dash_btn};
    for (int i = 0; i < 3; i++) {
        if (!objs[i]) continue;
        lv_obj_set_style_bg_color(objs[i], bg, LV_STATE_DISABLED);
        lv_obj_set_style_opa(objs[i], LV_OPA_COVER, LV_STATE_DISABLED);
        lv_obj_set_style_shadow_opa(objs[i], LV_OPA_TRANSP, LV_STATE_DISABLED);
        lv_obj_set_style_color_filter_opa(objs[i], LV_OPA_TRANSP, LV_STATE_DISABLED);   /* theme's grey-out */
        lv_obj_set_style_text_color(lv_obj_get_child(objs[i], 0), busy ? fg : lv_color_white(), 0);
        if (busy) lv_obj_add_state(objs[i], LV_STATE_DISABLED);
        else lv_obj_clear_state(objs[i], LV_STATE_DISABLED);
    }
}

/* Wi-Fi icon: green = connected, grey = not. HA dot: grey = not connected yet, green = HA answered, red = HA error; it pulses while connecting or reading.
 * Must be called with the LVGL lock held. */

/* HA dot fades in and out while connecting (no Wi-Fi) or while data is being read */
static bool s_net_busy, s_offline = true, s_ha_ok;
static void dot_opa_cb(void *obj, int32_t v) { lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0); }
/* Busy flips on and off with every request (many at start-up): the pulse and the fade-out
 * wait until it has been quiet for STATUS_SETTLE_MS, so the dot doesn't blink */
#define STATUS_SETTLE_MS 1500
static lv_timer_t *s_settle_timer;
static void dot_pulse(bool on)
{
    for (int i = 0; i < 2; i++) {
        lv_obj_t *d = s_ha_icon[i];
        if (!d) continue;
        bool running = lv_anim_get(d, dot_opa_cb) != NULL;
        if (on && !running) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, d);
            lv_anim_set_exec_cb(&a, dot_opa_cb);
            lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
            lv_anim_set_time(&a, 500);
            lv_anim_set_playback_time(&a, 500);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
            lv_anim_start(&a);
        } else if (!on && running) {
            lv_anim_del(d, dot_opa_cb);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        }
    }
}

static void settle_cb(lv_timer_t *t)
{
    (void)t;
    lv_timer_pause(s_settle_timer);
    dot_pulse(false);
    s_status_quiet = s_ha_ok;
    status_icons_update();
}

static void update_dot_pulse(void)
{
    bool on = s_net_busy || s_offline;
    s_wifi_quiet = !s_offline;
    if (!s_settle_timer) {
        s_settle_timer = lv_timer_create(settle_cb, STATUS_SETTLE_MS, NULL);
        lv_timer_pause(s_settle_timer);
    }
    if (on) {
        lv_timer_pause(s_settle_timer);
        s_status_quiet = false;
        dot_pulse(true);
    } else {
        if (!s_ha_ok) dot_pulse(false);   /* error: red dot, steady */
        s_status_quiet = false;           /* fades (if fine) once settled */
        lv_timer_reset(s_settle_timer);
        lv_timer_resume(s_settle_timer);
    }
    status_icons_update();
}

static void ha_busy(bool busy)
{
    if (!lvgl_port_lock(0)) return;
    s_net_busy = busy;
    update_dot_pulse();
    lvgl_port_unlock();
}

static void show_status(ha_state_t state)
{
    s_offline = state == HA_STATE_OFFLINE;
    /* darker shades on the light background for contrast */
    const uint32_t ok  = s_light ? 0x0B4D22 : 0x2ECC71;
    const uint32_t bad = s_light ? 0x8E0E0E : 0xE74C3C;
    const uint32_t off = s_light ? 0x2B2E33 : 0x8A8F98;
    bool wifi = state != HA_STATE_OFFLINE;
    uint32_t ha = !wifi ? off : state == HA_STATE_ERROR ? bad : ok;   /* grey until connected */
    for (int i = 0; i < 2; i++) {
        if (!s_wifi_icon[i]) continue;
        lv_obj_set_style_text_color(s_wifi_icon[i], lv_color_hex(wifi ? ok : off), 0);
        lv_obj_set_style_bg_color(s_ha_icon[i], lv_color_hex(ha), 0);
    }
    s_ha_ok = wifi && state != HA_STATE_ERROR;   /* dot is green */
    update_dot_pulse();
}

/* Network task: the request finished and the state was re-read */
static void ha_update_done(bool ok, bool refresh)
{
    if (!lvgl_port_lock(0)) return;
    /* the local changes reached HA (or failed: the next poll brings HA's values back) */
    s_theme_pending = s_orient_pending = s_bri_pending = s_pres_pending = false;
    if (!refresh) { lvgl_port_unlock(); return; }   /* a refresh still on its way stays busy */
    set_busy(false);
    if (s_refreshing) { /* errors show in the panel's error box, not on the buttons */
        s_refreshing = false;
        lv_label_set_text(lv_obj_get_child(s_refresh_btn, 0), LV_SYMBOL_REFRESH " Refresh");
    }
    s_view_pending = false;
    view_btn_update();
    dash_btn_update();
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

/* Theme button: switch at once, then tell Home Assistant (entity on = dark) */
static void ha_toggle_cb(lv_event_t *e)
{
    if (s_swiped) return;
    s_light = !s_light;
    s_theme_known = true;
    s_theme_pending = true;           /* HA's old state from a poll under way is ignored */
    apply_theme();
    ha_client_set_theme(!s_light);
}

/* Entity on = dark theme, off = light theme. Offline/error keeps the current theme;
 * so does a state read while the button's change is still on its way. */
static void ha_state_changed(ha_state_t state)
{
    if (!lvgl_port_lock(0)) return;
    if (!s_theme_pending && (state == HA_STATE_ON || state == HA_STATE_OFF)) {
        s_light = state == HA_STATE_OFF;
        s_theme_known = true;
    }
    if (state == HA_STATE_UNKNOWN && !s_theme_pending) s_theme_known = false;
    s_offline_state = state == HA_STATE_OFFLINE;
    apply_theme();
    show_status(state);
    lvgl_port_unlock();
}

/* Colours of the screen and panel for s_light, and the theme button's label.
 * Must be called with the LVGL lock held. */
static void apply_theme(void)
{
    if (!s_ha_btn) return;
    lv_obj_t *label = lv_obj_get_child(s_ha_btn, 0);
    bool light = s_light;
    /* Light: white background, dark button, darker slider. Dark: black background. */
    set_screen_bg(lv_scr_act(), light);
    lv_obj_set_style_bg_color(s_ha_btn,
        light ? lv_color_hex(0x2C3440) : lv_color_hex(0x5E5E6E), 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    apply_panel_theme(light);
    apply_temp_theme(light);
    if (s_orient_btn) lv_obj_set_style_bg_color(s_orient_btn, neutral_btn_color(), 0);
    if (s_view_btn) lv_obj_set_style_bg_color(s_view_btn, neutral_btn_color(), 0);
    if (s_refresh_btn) lv_obj_set_style_bg_color(s_refresh_btn, neutral_btn_color(), 0);
    if (s_refresh_btn && !s_refreshing) { /* follow theme */
        lv_label_set_text(lv_obj_get_child(s_refresh_btn, 0),
                          s_offline_state ? "Connecting..." : LV_SYMBOL_REFRESH " Refresh");
    }
    if (s_slider) {
        lv_color_t accent = lv_color_hex(light ? 0x0B7FAB : 0x18BCF2);
        lv_obj_set_style_bg_color(s_slider, lv_color_hex(light ? 0xAEB4BE : 0x3A3F47), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_slider, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_slider, accent, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_slider, accent, LV_PART_KNOB);
    }
    lv_label_set_text(label, !s_theme_known ? "Theme" : light ? "Dark mode" : "Light mode");
    set_busy(s_busy);                 /* disabled colours follow the theme */
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
    if (!s_bri_pending) apply_backlight(value);

    if (!s_slider || !lvgl_port_lock(0)) return;
    /* don't fight a drag in progress, nor a value still on its way to HA */
    if (!s_bri_pending && !lv_obj_has_state(s_slider, LV_STATE_PRESSED)) {
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
    apply_backlight(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        s_bri_pending = true;
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
        else if (PAGE_COUNT) show_page((s_page + 1) % PAGE_COUNT);
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
    ui_refill();
    lv_obj_invalidate(lv_scr_act());
}

/* Network task: orientation entity state */
static void ha_landscape_changed(bool landscape)
{
    if (!lvgl_port_lock(0)) return;
    if (!s_orient_pending && landscape != s_landscape) lv_async_call(orient_apply, (void *)(intptr_t)landscape);
    lvgl_port_unlock();
}

/* View button: the current view's title; "Loading..." while a change is on its way.
 * Must be called with the LVGL lock held. */
static void view_btn_update(void)
{
    if (!s_view_btn) return;
    lv_obj_t *l = lv_obj_get_child(s_view_btn, 0);
    if (s_view_pending) lv_label_set_text(l, "Loading...");
    else if (s_cur_view >= 0) lv_label_set_text_fmt(l, "%s " LV_SYMBOL_DOWN, s_views[s_cur_view].title);
    else lv_label_set_text(l, s_nviews ? "- " LV_SYMBOL_DOWN : "Connecting...");
}

/* Network task: the dashboard's views and the one shown */
static void ha_views_changed(const layout_view_t *views, int n, int current)
{
    if (!lvgl_port_lock(0)) return;
    if (n != s_nviews || current != s_cur_view || memcmp(s_views, views, n * sizeof(*views)))
        view_menu_close();            /* the open list is out of date */
    memcpy(s_views, views, n * sizeof(*views));
    s_nviews = n;
    s_cur_view = current;
    view_btn_update();
    lvgl_port_unlock();
}

static void view_menu_close(void)
{
    if (s_view_menu) lv_obj_del(s_view_menu);
    s_view_menu = NULL;
}

static void view_menu_bg_cb(lv_event_t *e) { view_menu_close(); }   /* tap beside the list */

/* Show view i: its dashboard sections become the main pages (from the first one) and the
 * panel closes once they are there; remembered across reboots. Also the serial 'V' command. */
static void view_select(int i)
{
    view_menu_close();
    if (s_busy || i < 0 || i >= s_nviews || i == s_cur_view) return;
    ESP_LOGI(TAG, "view %d '%s'", i, s_views[i].path);
    snprintf(s_view_path, sizeof(s_view_path), "%s", s_views[i].path);
    nvs_handle_t nv;
    if (nvs_open("ui", NVS_READWRITE, &nv) == ESP_OK) {
        nvs_set_str(nv, "view", s_view_path);
        nvs_commit(nv);
        nvs_close(nv);
    }
    s_view_pending = true;
    set_busy(true);
    view_btn_update();
    ha_client_set_view(s_view_path);
    s_ha_page = 0;
    ha_client_set_nav(s_dash_path, s_view_path, 0);
}

static void view_pick_cb(lv_event_t *e) { view_select((int)(intptr_t)lv_event_get_user_data(e)); }

/* A list of items below btn (above, if there's no room); the current one is ticked, pick
 * gets the index as user data. The list scrolls when it's taller than the screen allows. */
static void menu_open(lv_obj_t *btn, const layout_view_t *items, int n, int cur, lv_event_cb_t pick)
{
    /* backdrop over the whole panel: catches taps beside the list and keeps swipes off it */
    lv_obj_t *bg = lv_obj_create(s_cfg);
    lv_obj_remove_style_all(bg);
    lv_obj_set_size(bg, CFG_W, SCR_H);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bg, view_menu_bg_cb, LV_EVENT_CLICKED, NULL);
    s_view_menu = bg;

    const lv_coord_t row_h = 40, w = 148;
    lv_area_t a;
    lv_obj_get_coords(btn, &a);
    lv_coord_t below = SCR_H - a.y2 - 6, above = a.y1 - STATUS_H;
    lv_coord_t want = n * row_h + 8;
    bool down = below >= want || below >= above;
    lv_coord_t h = LV_MIN(want, down ? below : above);

    lv_obj_t *box = lv_obj_create(bg);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, a.x1, down ? a.y2 + 4 : a.y1 - 4 - h);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 4, 0);
    lv_obj_set_style_pad_row(box, 0, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(s_light ? 0xC8CDD4 : 0x3A3F47), 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(s_light ? 0xFFFFFF : 0x24282F), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_ACTIVE);

    lv_color_t text = s_light ? lv_color_hex(0x1C1C1E) : lv_color_white();
    for (int i = 0; i < n; i++) {
        lv_obj_t *r = lv_obj_create(box);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, lv_pct(100), row_h);
        lv_obj_set_style_radius(r, 8, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(0x18BCF2), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_40, LV_STATE_PRESSED);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_add_event_cb(r, pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(r);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, w - 8 - 36);
        lv_label_set_text(l, items[i].title);
        lv_obj_set_style_text_font(l, &lv_font_latin_16, 0);
        lv_obj_set_style_text_color(l, text, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
        if (i == cur) {
            lv_obj_t *c = lv_label_create(r);
            lv_label_set_text(c, LV_SYMBOL_OK);
            lv_obj_set_style_text_font(c, &lv_font_latin_16, 0);
            lv_obj_set_style_text_color(c, lv_color_hex(0x18BCF2), 0);
            lv_obj_align(c, LV_ALIGN_RIGHT_MID, -8, 0);
        }
    }
    if (cur >= 0) lv_obj_scroll_to_view(lv_obj_get_child(box, cur), LV_ANIM_OFF);
}

/* View button: the dashboard's views */
static void view_cb(lv_event_t *e)
{
    if (s_busy || s_swiped || s_nviews < 1 || s_view_menu) return;
    menu_open(s_view_btn, s_views, s_nviews, s_cur_view, view_pick_cb);
}

/* Dashboard button: the current dashboard's title; "Loading..." while a change is on its way.
 * Must be called with the LVGL lock held. */
static void dash_btn_update(void)
{
    if (!s_dash_btn) return;
    lv_obj_t *l = lv_obj_get_child(s_dash_btn, 0);
    if (s_view_pending) lv_label_set_text(l, "Loading...");
    else if (s_cur_dash >= 0) lv_label_set_text_fmt(l, "%s " LV_SYMBOL_DOWN, s_dashes[s_cur_dash].title);
    else lv_label_set_text(l, s_ndash ? HA_DASHBOARD " " LV_SYMBOL_DOWN : "Connecting...");
}

/* Network task: HA's dashboards and the one shown */
static void ha_dashes_changed(const layout_view_t *dashes, int n, int current)
{
    if (!lvgl_port_lock(0)) return;
    if (n != s_ndash || current != s_cur_dash || memcmp(s_dashes, dashes, n * sizeof(*dashes)))
        view_menu_close();
    memcpy(s_dashes, dashes, n * sizeof(*dashes));
    s_ndash = n;
    s_cur_dash = current;
    dash_btn_update();
    lvgl_port_unlock();
}

/* Show dashboard i from its first view; remembered across reboots */
static void dash_select(int i)
{
    view_menu_close();
    if (s_busy || i < 0 || i >= s_ndash || i == s_cur_dash) return;
    ESP_LOGI(TAG, "dashboard %d '%s'", i, s_dashes[i].path);
    snprintf(s_dash_path, sizeof(s_dash_path), "%s", s_dashes[i].path);
    s_view_path[0] = 0;
    nvs_handle_t nv;
    if (nvs_open("ui", NVS_READWRITE, &nv) == ESP_OK) {
        nvs_set_str(nv, "dash", s_dash_path);
        nvs_set_str(nv, "view", s_view_path);
        nvs_commit(nv);
        nvs_close(nv);
    }
    s_cur_view = -1;
    s_view_pending = true;
    set_busy(true);
    view_btn_update();
    dash_btn_update();
    ha_client_set_dashboard(s_dash_path, s_view_path);
    s_ha_page = 0;
    ha_client_set_nav(s_dash_path, s_view_path, 0);
}

/* Dashboard paths: "" and HA_DASHBOARD are the same one */
static bool dash_same(const char *a, const char *b)
{
    if (!a[0]) a = HA_DASHBOARD;
    if (!b[0]) b = HA_DASHBOARD;
    return !strcmp(a, b);
}

/* HA's dashboard / view / page entities (LVGL task): follow them when they differ */
static struct { char dash[32], view[32]; bool dash_ok, view_ok; int page; bool queued; } s_nav_in;

static void nav_apply(void *arg);
static void nav_retry_cb(lv_timer_t *t) { nav_apply(NULL); }

static void nav_apply(void *arg)
{
    s_nav_in.queued = false;
    if (s_view_pending || s_busy) {                /* a change or refresh is under way: later */
        s_nav_in.queued = true;
        lv_timer_t *t = lv_timer_create(nav_retry_cb, 1000, NULL);
        lv_timer_set_repeat_count(t, 1);
        return;
    }
    const char *dash = s_nav_in.dash_ok ? s_nav_in.dash : s_dash_path;
    const char *view = s_nav_in.view_ok ? s_nav_in.view : s_view_path;
    int page = s_nav_in.page;
    if (page >= 0) s_ha_page = page;
    bool dash_new = !dash_same(dash, s_dash_path);
    if (dash_new || strcmp(view, s_view_path)) {
        ESP_LOGI(TAG, "HA: dashboard '%s', view '%s', page %d", dash, view, page + 1);
        if (dash_new) snprintf(s_dash_path, sizeof(s_dash_path), "%s", dash);
        snprintf(s_view_path, sizeof(s_view_path), "%s", view);
        nvs_handle_t nv;
        if (nvs_open("ui", NVS_READWRITE, &nv) == ESP_OK) {
            nvs_set_str(nv, "dash", s_dash_path);
            nvs_set_str(nv, "view", s_view_path);
            nvs_commit(nv);
            nvs_close(nv);
        }
        if (dash_new) s_cur_dash = -1;
        s_cur_view = -1;
        s_view_pending = true;
        s_boot_page = page >= 0 ? page : -1;      /* applied once the new pages exist */
        set_busy(true);
        view_btn_update();
        dash_btn_update();
        ha_client_set_dashboard(s_dash_path, s_view_path);
    } else if (page >= 0 && !PAGE_COUNT) {
        s_boot_page = page;                        /* no pages yet */
    } else if (page >= 0 && page != s_page) {
        ESP_LOGI(TAG, "HA: page %d", page + 1);
        if (s_pres_on) pres_touch();              /* stay there for a full interval */
        show_page(page);
    }
}

/* Network task */
static void ha_nav_changed(const char *dash, const char *view, int page)
{
    if (!lvgl_port_lock(0)) return;
    s_nav_in.dash_ok = dash != NULL;
    s_nav_in.view_ok = view != NULL;
    snprintf(s_nav_in.dash, sizeof(s_nav_in.dash), "%s", dash ? dash : "");
    snprintf(s_nav_in.view, sizeof(s_nav_in.view), "%s", view ? view : "");
    s_nav_in.page = page;
    if (!s_nav_in.queued) { s_nav_in.queued = true; lv_async_call(nav_apply, NULL); }
    lvgl_port_unlock();
}

static void dash_pick_cb(lv_event_t *e) { dash_select((int)(intptr_t)lv_event_get_user_data(e)); }

static void dash_cb(lv_event_t *e)
{
    if (s_busy || s_swiped || s_ndash < 1 || s_view_menu) return;
    menu_open(s_dash_btn, s_dashes, s_ndash, s_cur_dash, dash_pick_cb);
}

/* After a rebuild: the theme, the refresh/view busy state and the last known values of every
 * card, instead of empty cards until HA answers again. Must be called with the LVGL lock held. */
static void ui_refill(void)
{
    apply_theme();
    set_busy(s_busy);
    if (s_refreshing) lv_label_set_text(lv_obj_get_child(s_refresh_btn, 0), "Refreshing...");
    ha_client_replay();
}

/* Orientation button: toggle the HA entity; the layout follows when HA reports it */
static void orient_cb(lv_event_t *e)
{
    if (s_swiped) return;
    bool want = !s_landscape;
    s_orient_pending = true;          /* HA's old state from a poll under way is ignored */
    ha_client_set_landscape(want);
    lv_async_call(orient_apply, (void *)(intptr_t)want);   /* this button is deleted by it */
}

/* Network task: a new layout from the dashboard (or the NVS cache). Rebuilds the UI right
 * away, before the values for it arrive; keeps the current page when it still exists. */
/* Network task: a card's icon changed */
static void ha_icon_changed(int page, int row, const char *icon)
{
    if (!lvgl_port_lock(0)) return;
    if (page >= 0 && page < LAYOUT_MAX_PAGES && row >= 0 && row < LAYOUT_MAX_ROWS) {
        snprintf(s_icon_name[page][row], sizeof(s_icon_name[page][row]), "%s", icon);
        if (page < PAGE_COUNT && row < ROWS(page)) {
            icon_show(page, row);
            if (KIND(page, row) == LAYOUT_VALUES) icon_place(page, row);   /* hidden again if no room */
        }
    }
    lvgl_port_unlock();
}

static void ha_layout_changed(const ha_layout_t *layout)
{
    if (!lvgl_port_lock(0)) return;
    bool was_open = s_cfg_open && !s_view_pending;   /* a new view: show its pages */
    if (s_view_pending) s_page = 0;
    s_layout = *layout;
    memset(s_icon_name, 0, sizeof(s_icon_name));   /* rows changed: HA sends them again */
    create_ui();   /* keeps the current page when it still exists */
    if (was_open) { lv_obj_set_y(s_cfg, 0); s_cfg_open = true; }
    ui_refill();
    lv_obj_invalidate(lv_scr_act());
    lvgl_port_unlock();
}

static void test_tap(int x, int y)
{
    ESP_LOGI(TAG, "test tap %d,%d", x, y);
    if (s_cfg_open) {                     /* config panel: click the object under the point */
        lv_point_t p = { x, y };
        lv_obj_t *o = lv_indev_search_obj(s_cfg, &p);
        while (o && !lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE)) o = lv_obj_get_parent(o);
        if (o) lv_event_send(o, LV_EVENT_CLICKED, NULL);
        return;
    }
    value_tap((lv_point_t){ x, y });
}
