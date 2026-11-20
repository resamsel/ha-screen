/* Host tests for main/layout.c. Build/run: see test/run.sh */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "layout.h"

static char *read_file(const char *p)
{
    FILE *f = fopen(p, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *b = malloc(n + 1);
    assert(fread(b, 1, n, f) == (size_t)n);
    b[n] = 0;
    fclose(f);
    return b;
}

/* wraps sections JSON into a bare dashboard config with view "main" */
static int parse_sections(const char *sections, ha_layout_t *l)
{
    static char buf[16384];
    snprintf(buf, sizeof(buf), "{\"views\":[{\"path\":\"main\",\"type\":\"sections\",\"sections\":%s}]}", sections);
    return layout_parse(buf, "main", l);
}

#define TILE(e) "{\"type\":\"tile\",\"entity\":\"" e "\"}"
#define HEAD(t) "{\"type\":\"heading\",\"heading\":\"" t "\"}"
#define GRAPH(e) "{\"type\":\"sensor\",\"graph\":\"line\",\"entity\":\"" e "\"}"
#define BTN(e) "{\"type\":\"button\",\"entity\":\"" e "\"}"
#define SEC(...) "{\"type\":\"grid\",\"cards\":[" __VA_ARGS__ "]}"

static void test_fixture(void)
{
    ha_layout_t l;
    char *j = read_file("test/fixtures/dashboard.json");
    assert(layout_parse(j, "main", &l) == 5);
    assert(l.count == 5);
    assert(l.pages[0].kind == LAYOUT_VALUES);
    assert(!strcmp(l.pages[0].title, "Temperatur"));
    assert(l.pages[0].count == 3);
    assert(!strcmp(l.pages[0].items[0].entity, "sensor.temperature_og"));
    assert(!strcmp(l.pages[0].items[0].name, ""));
    assert(l.pages[2].kind == LAYOUT_GRAPH);
    assert(!strcmp(l.pages[2].title, "Paketverlust"));
    assert(l.pages[2].count == 1);
    assert(l.pages[2].items[0].hours == 24);
    assert(!strcmp(l.pages[4].title, "New section"));
    assert(!strcmp(l.pages[4].items[0].entity, "sensor.fritz_box_7590_ax_download_geschwindigkeit"));
    free(j);
}

static void test_name(void)
{
    ha_layout_t l;
    assert(parse_sections("[" SEC("{\"type\":\"tile\",\"entity\":\"sensor.a\",\"name\":{\"type\":\"device\"}}" ","
                                  "{\"type\":\"tile\",\"entity\":\"sensor.b\",\"name\":\"Ping\"}" ","
                                  "{\"type\":\"tile\",\"entity\":\"sensor.c\",\"name\":[{\"type\":\"floor\"},{\"type\":\"text\",\"text\":\"x\"},{\"type\":\"entity\"},7]}" ","
                                  "{\"type\":\"tile\",\"entity\":\"sensor.d\",\"name\":{\"type\":\"text\",\"text\":\"Hi\"}}") "]", &l) == 1);
    assert(!strcmp(l.pages[0].items[0].name, ""));
    assert(!strcmp(l.pages[0].items[0].name_type, "device"));
    assert(!strcmp(l.pages[0].items[1].name, "Ping"));
    assert(!strcmp(l.pages[0].items[1].name_type, ""));
    assert(!strcmp(l.pages[0].items[2].name_type, "floor|text:x|entity"));
    assert(!strcmp(l.pages[0].items[3].name, "Hi"));
    assert(!strcmp(l.pages[0].items[3].name_type, ""));
}

static void test_name_template(void)
{
    layout_item_t it = { .entity = "sensor.a", .name_type = "floor" };
    char t[1024];
    assert(layout_name_template(&it, t, sizeof(t)));
    assert(strstr(t, "floor_name('sensor.a')"));
    strcpy(it.name_type, "area|device");
    assert(layout_name_template(&it, t, sizeof(t)));
    assert(strstr(t, "area_name('sensor.a')") && strstr(t, "device_attr('sensor.a', 'name')"));
    strcpy(it.name_type, "entity");
    assert(layout_name_template(&it, t, sizeof(t)));
    assert(strstr(t, "friendly_name"));
    strcpy(it.name_type, "text:Hallo");
    assert(layout_name_template(&it, t, sizeof(t)) && strstr(t, "Hallo"));
    strcpy(it.name_type, "bogus");
    assert(!layout_name_template(&it, t, sizeof(t)));   /* nothing known: no template */
    strcpy(it.name_type, "");
    assert(!layout_name_template(&it, t, sizeof(t)));
}

static void test_hours(void)
{
    ha_layout_t l;
    assert(parse_sections("[" SEC("{\"type\":\"sensor\",\"graph\":\"line\",\"entity\":\"sensor.a\",\"hours_to_show\":6}")
                          "," SEC("{\"type\":\"sensor\",\"graph\":\"line\",\"entity\":\"sensor.a\",\"hours_to_show\":0}")
                          "," SEC("{\"type\":\"sensor\",\"graph\":\"line\",\"entity\":\"sensor.a\",\"hours_to_show\":\"x\"}")
                          "," SEC("{\"type\":\"sensor\",\"graph\":\"line\",\"entity\":\"sensor.a\",\"hours_to_show\":500}") "]", &l) == 4);
    assert(l.pages[0].items[0].hours == 6);
    assert(l.pages[1].items[0].hours == 24);
    assert(l.pages[2].items[0].hours == 24);
    assert(l.pages[3].items[0].hours == 168);
}

static void test_skips_unknown(void)
{
    ha_layout_t l;
    assert(parse_sections("[" SEC("{\"type\":\"markdown\",\"content\":\"hi\"}") "]", &l) == 0);
    assert(parse_sections("[{\"type\":\"weird\",\"cards\":[" TILE("sensor.a") "]}," SEC(TILE("sensor.b")) "]", &l) == 1);
    assert(!strcmp(l.pages[0].items[0].entity, "sensor.b"));
    assert(parse_sections("[" SEC("{\"type\":\"tile\"}" "," "{\"type\":\"tile\",\"entity\":5}" "," TILE("sensor.c")) "]", &l) == 1);
    assert(l.pages[0].count == 1);
    assert(parse_sections("5", &l) == 0);
    assert(parse_sections("[5,\"x\",null,{\"cards\":7}]", &l) == 0);
}

static void test_no_heading(void)
{
    ha_layout_t l;
    assert(parse_sections("[" SEC(TILE("sensor.a")) "]", &l) == 1);
    assert(!strcmp(l.pages[0].title, ""));
}

static void test_graph_first_wins(void)
{
    ha_layout_t l;
    assert(parse_sections("[" SEC(GRAPH("sensor.g") "," TILE("sensor.t")) "]", &l) == 1);
    assert(l.pages[0].kind == LAYOUT_GRAPH);
    assert(l.pages[0].count == 2);   /* mixed: both cards, each with its own kind */
    assert(l.pages[0].items[0].kind == LAYOUT_GRAPH && l.pages[0].items[1].kind == LAYOUT_VALUES);
    assert(!strcmp(l.pages[0].items[0].entity, "sensor.g"));
    assert(parse_sections("[" SEC("{\"type\":\"sensor\",\"entity\":\"sensor.s\"}") "]", &l) == 0);
}

static void test_several_graphs(void)
{
    ha_layout_t l;
    assert(parse_sections("[" SEC(HEAD("Netz") "," GRAPH("sensor.a") "," TILE("sensor.t") ","
                                  "{\"type\":\"sensor\",\"graph\":\"line\",\"entity\":\"sensor.b\",\"hours_to_show\":6}") "]", &l) == 1);
    assert(l.pages[0].kind == LAYOUT_GRAPH && l.pages[0].count == 3);
    assert(!strcmp(l.pages[0].items[2].entity, "sensor.b"));
    assert(l.pages[0].items[0].hours == 24 && l.pages[0].items[2].hours == 6);
    assert(l.pages[0].items[1].kind == LAYOUT_VALUES && l.pages[0].items[1].hours == 0);
}

static void test_limits(void)
{
    ha_layout_t l;
    const char *s = SEC(TILE("sensor.a"));
    char buf[4096] = "[";
    for (int i = 0; i < 14; i++) { strcat(buf, s); if (i < 13) strcat(buf, ","); }
    strcat(buf, "]");
    assert(parse_sections(buf, &l) == 12);   /* 14 sections, limit 12 */
    assert(parse_sections("[" SEC(TILE("sensor.1") "," TILE("sensor.2") "," TILE("sensor.3") "," TILE("sensor.4") ","
                                  TILE("sensor.5") "," TILE("sensor.6")) "]", &l) == 1);
    assert(l.pages[0].count == 4);
}

static void test_bad_input(void)
{
    ha_layout_t l;
    assert(layout_parse("", "main", &l) == 0);
    assert(layout_parse("{", "main", &l) == 0);
    assert(layout_parse("null", "main", &l) == 0);
    assert(layout_parse(NULL, "main", &l) == 0);
    assert(layout_parse("{\"views\":[{\"path\":\"other\",\"sections\":[" SEC(TILE("sensor.a")) "]}]}", "main", &l) == 0);
    assert(l.count == 0);
    {   /* view list: title or path, index for a view without path; missing view = first */
        layout_view_t v[LAYOUT_MAX_VIEWS];
        int nv;
        char ch[32];
        const char *j = "{\"views\":[{\"path\":\"a\",\"title\":\"Home\",\"sections\":[" SEC(TILE("sensor.a")) "]},"
                        "{\"sections\":[" SEC(TILE("sensor.b")) "," SEC(TILE("sensor.c")) "]}]}";
        assert(layout_parse_views(j, "", &l, v, LAYOUT_MAX_VIEWS, &nv, ch) == 1);
        assert(nv == 2 && !strcmp(v[0].title, "Home") && !strcmp(v[1].path, "1") && !strcmp(ch, "a"));
        assert(layout_parse_views(j, "1", &l, v, LAYOUT_MAX_VIEWS, &nv, ch) == 2 && !strcmp(ch, "1"));
        assert(layout_parse_views(j, "gone", &l, v, LAYOUT_MAX_VIEWS, &nv, ch) == 1 && !strcmp(ch, "a"));
    }
}

static void test_hash(void)
{
    ha_layout_t a, b;
    parse_sections("[" SEC(HEAD("T") "," TILE("sensor.a")) "]", &a);
    parse_sections("[" SEC(HEAD("T") "," TILE("sensor.a")) "]", &b);
    assert(layout_hash(&a) == layout_hash(&b));
    parse_sections("[" SEC(HEAD("T") "," TILE("sensor.b")) "]", &b);
    assert(layout_hash(&a) != layout_hash(&b));
}

static bool keep_not_x(const char *entity, layout_kind_t kind, void *ctx)
{
    (void)ctx;
    (void)kind;
    return strcmp(entity, "sensor.x") != 0;
}

static void test_filter(void)
{
    ha_layout_t l;
    parse_sections("[" SEC(TILE("sensor.a") "," TILE("sensor.x") "," TILE("sensor.b")) "," SEC(GRAPH("sensor.x"))
                   "," SEC(GRAPH("sensor.g")) "]", &l);
    assert(layout_filter(&l, keep_not_x, NULL) == 2);
    assert(l.count == 2);
    assert(l.pages[0].count == 2);
    assert(!strcmp(l.pages[0].items[1].entity, "sensor.b"));
    assert(!strcmp(l.pages[1].items[0].entity, "sensor.g"));
}

static void test_sanitize(void)
{
    ha_layout_t l;
    parse_sections("[" SEC(GRAPH("sensor.g")) "," SEC(TILE("sensor.a")) "]", &l);
    uint32_t before = layout_hash(&l);
    assert(layout_sanitize(&l));
    assert(layout_hash(&l) == before);   /* a valid layout comes back unchanged (cache hit) */
    l.pages[0].items[0].hours = 0;
    l.pages[1].kind = 7;
    assert(layout_sanitize(&l));
    assert(l.pages[0].items[0].hours == LAYOUT_HOURS_DEFAULT);
    assert(l.pages[1].kind == LAYOUT_VALUES);
    l.pages[0].items[0].hours = 9999;
    assert(layout_sanitize(&l) && l.pages[0].items[0].hours == LAYOUT_HOURS_MAX);
    l.pages[1].count = 9;
    assert(!layout_sanitize(&l) && l.count == 0);
    l.count = 42;
    assert(!layout_sanitize(&l));
}

static void test_buttons(void)
{
    ha_layout_t l;
    assert(parse_sections("[" SEC(HEAD("Knöpfe") "," BTN("switch.a") "," TILE("sensor.t") ","
                                  "{\"type\":\"button\",\"entity\":\"script.s\",\"tap_action\":{\"action\":\"more-info\"}}" ","
                                  "{\"type\":\"button\",\"entity\":\"scene.x\",\"tap_action\":{\"action\":\"toggle\"}}" ","
                                  "{\"type\":\"button\"}") "]", &l) == 1);
    assert(l.pages[0].kind == LAYOUT_BUTTONS);
    assert(l.pages[0].count == 4);                       /* entity-less button skipped */
    assert(!strcmp(l.pages[0].items[0].entity, "switch.a"));
    assert(l.pages[0].items[0].action == LAYOUT_ACT_TOGGLE);
    assert(l.pages[0].items[1].kind == LAYOUT_VALUES);   /* the tile stays, as its own block */
    assert(l.pages[0].items[2].action == LAYOUT_ACT_NONE);
    assert(l.pages[0].items[3].action == LAYOUT_ACT_TOGGLE);
    assert(parse_sections("[" SEC(TILE("sensor.t") "," BTN("switch.a")) "]", &l) == 1);
    assert(l.pages[0].kind == LAYOUT_VALUES && l.pages[0].count == 2 && l.pages[0].items[1].kind == LAYOUT_BUTTONS);
    /* sanitize keeps button pages */
    parse_sections("[" SEC(BTN("switch.a")) "]", &l);
    uint32_t h = layout_hash(&l);
    assert(layout_sanitize(&l) && layout_hash(&l) == h && l.pages[0].kind == LAYOUT_BUTTONS);
}

static layout_kind_t s_seen_kind;
static bool keep_record(const char *entity, layout_kind_t kind, void *ctx)
{
    (void)entity; (void)ctx;
    s_seen_kind = kind;
    return true;
}

static void test_filter_kind(void)
{
    ha_layout_t l;
    parse_sections("[" SEC(BTN("switch.a")) "]", &l);
    layout_filter(&l, keep_record, NULL);
    assert(s_seen_kind == LAYOUT_BUTTONS);
}

static void test_clock(void)
{
    ha_layout_t l;
    assert(parse_sections("[" SEC(HEAD("Zeit") "," "{\"type\":\"clock\",\"clock_size\":\"large\",\"show_seconds\":true,\"time_format\":\"12\"}" ","
                                  TILE("sensor.t")) "," SEC("{\"type\":\"clock\"}") "]", &l) == 2);
    assert(l.pages[0].kind == LAYOUT_CLOCK && l.pages[0].count == 2);   /* clock + tile */
    assert(l.pages[0].items[0].kind == LAYOUT_CLOCK && l.pages[0].items[1].kind == LAYOUT_VALUES);
    assert(!strcmp(l.pages[0].title, "Zeit"));
    assert(l.pages[0].items[0].flags == (LAYOUT_CLOCK_SECONDS | LAYOUT_CLOCK_12H | LAYOUT_CLOCK_LARGE));
    assert(l.pages[0].items[0].entity[0] == 0);
    assert(l.pages[1].kind == LAYOUT_CLOCK && l.pages[1].items[0].flags == 0);
    assert(parse_sections("[" SEC("{\"type\":\"clock\",\"clock_size\":\"large\",\"no_background\":true,\"time_zone\":\"Europe/London\"}")
                          "," SEC("{\"type\":\"clock\",\"clock_size\":\"medium\"}") "]", &l) == 2);
    assert((l.pages[0].items[0].flags & LAYOUT_CLOCK_SIZE) == LAYOUT_CLOCK_LARGE);
    assert(l.pages[0].items[0].flags & LAYOUT_CLOCK_NO_BG);
    assert(!strcmp(l.pages[0].items[0].entity, "Europe/London"));   /* clock: time zone */
    assert((l.pages[1].items[0].flags & LAYOUT_CLOCK_SIZE) == LAYOUT_CLOCK_MEDIUM);
    assert(!(l.pages[1].items[0].flags & LAYOUT_CLOCK_NO_BG) && !l.pages[1].items[0].entity[0]);
    /* filter never drops a clock (no entity to check) */
    assert(layout_filter(&l, keep_not_x, NULL) == 2);
    uint32_t h = layout_hash(&l);
    assert(layout_sanitize(&l) && layout_hash(&l) == h);
}

static void test_picture(void)
{
    ha_layout_t l;
    assert(parse_sections("[" SEC("{\"type\":\"picture-entity\",\"entity\":\"camera.door\"}" ","
                                  "{\"type\":\"picture-entity\",\"entity\":\"image.map\",\"show_name\":false,\"name\":\"Map\"}" ","
                                  "{\"type\":\"picture-entity\"}") "]", &l) == 1);
    assert(l.pages[0].kind == LAYOUT_PICTURE && l.pages[0].count == 2);   /* no entity: skipped */
    assert(!strcmp(l.pages[0].items[0].entity, "camera.door") && l.pages[0].items[0].flags == 0);
    assert(l.pages[0].items[1].flags == LAYOUT_PIC_NO_NAME && !strcmp(l.pages[0].items[1].name, "Map"));
    uint32_t h = layout_hash(&l);
    assert(layout_sanitize(&l) && layout_hash(&l) == h);
    assert(parse_sections("[" SEC("{\"type\":\"picture-entity\",\"entity\":\"camera.a\",\"fit_mode\":\"cover\"}" ","
                                  "{\"type\":\"picture-entity\",\"entity\":\"camera.b\",\"fit_mode\":\"fill\"}" ","
                                  "{\"type\":\"picture-entity\",\"entity\":\"camera.c\",\"fit_mode\":\"contain\"}") "]", &l) == 1);
    assert(l.pages[0].items[0].flags == LAYOUT_PIC_COVER && l.pages[0].items[1].flags == LAYOUT_PIC_FILL && !l.pages[0].items[2].flags);
    /* UI editor stores the image as a media selector */
    ha_layout_t m;
    assert(parse_sections("[" SEC("{\"type\":\"picture-entity\",\"entity\":\"switch.a\",\"image\":{\"media_content_id\":\"https://x/a.png\"}}" ","
                                  "{\"type\":\"picture-entity\",\"entity\":\"switch.b\",\"image\":{\"media_content_id\":\"media-source://image/image.map\"}}") "]", &m) == 1);
    assert(!strcmp(m.images[m.pages[0].items[0].image - 1], "https://x/a.png"));
    assert(!strcmp(m.images[m.pages[0].items[1].image - 1], "image.map"));
    l.pages[0].items[2].flags = 0xFF;
    layout_sanitize(&l);
    assert(l.pages[0].items[2].flags == LAYOUT_PIC_FLAGS);
    /* own picture source: image URL or camera_image (preferred), shared between cards */
    assert(parse_sections("[" SEC("{\"type\":\"picture-entity\",\"entity\":\"switch.a\",\"image\":\"/local/a.png\"}" ","
                                  "{\"type\":\"picture-entity\",\"entity\":\"switch.b\",\"image\":\"/local/x.png\",\"camera_image\":\"camera.c\"}" ","
                                  "{\"type\":\"picture-entity\",\"entity\":\"switch.c\",\"image\":\"/local/a.png\"}") "]", &l) == 1);
    assert(l.pages[0].items[0].image == 1 && !strcmp(l.images[0], "/local/a.png"));
    assert(l.pages[0].items[1].image == 2 && !strcmp(l.images[1], "camera.c"));
    assert(l.pages[0].items[2].image == 1 && !l.images[2][0]);
    s_seen_kind = -1;
    layout_filter(&l, keep_record, NULL);
    assert(s_seen_kind == LAYOUT_VALUES);   /* entity only has to exist */
    assert(layout_sanitize(&l) && l.pages[0].items[1].image == 2);
    l.pages[0].items[0].image = 9;
    assert(layout_sanitize(&l) && l.pages[0].items[0].image == 0);
    l.pages[0].items[0].flags = 0xFF;
    assert(layout_sanitize(&l) && l.pages[0].items[0].flags == LAYOUT_PIC_FLAGS);
}

static void test_icons(void)
{
    ha_layout_t l;
    assert(parse_sections("[" SEC("{\"type\":\"tile\",\"entity\":\"sensor.a\",\"icon\":\"mdi:fire\"}" ","
                                  "{\"type\":\"button\",\"entity\":\"switch.b\",\"show_icon\":false,\"icon\":\"mdi:fire\"}" ","
                                  "{\"type\":\"tile\",\"entity\":\"sensor.c\"}") "]", &l) == 1);
    assert(!strcmp(l.pages[0].items[0].icon, "mdi:fire"));
    assert(!strcmp(l.pages[0].items[1].icon, "-"));
    assert(!l.pages[0].items[2].icon[0]);
}

static void test_heading_icon(void)
{
    ha_layout_t l;
    assert(parse_sections("[" SEC("{\"type\":\"heading\",\"heading\":\"Wetter\",\"icon\":\"mdi:weather-cloudy\"}" ","
                                  "{\"type\":\"tile\",\"entity\":\"sensor.a\"}") "]", &l) == 1);
    assert(!strcmp(l.pages[0].title, "Wetter") && !strcmp(l.pages[0].title_icon, "mdi:weather-cloudy"));
}

static void test_new_cards(void)
{
    ha_layout_t l;
    assert(parse_sections("[" SEC("{\"type\":\"entity\",\"entity\":\"sensor.a\"}" ","
                                  "{\"type\":\"gauge\",\"entity\":\"sensor.b\",\"min\":-10,\"max\":40,\"severity\":{\"green\":0,\"yellow\":25,\"red\":30}}" ","
                                  "{\"type\":\"gauge\",\"entity\":\"sensor.c\"}") "]", &l) == 1);
    assert(l.pages[0].items[0].kind == LAYOUT_VALUES);
    assert(l.pages[0].items[1].kind == LAYOUT_GAUGE && l.pages[0].items[1].min == -10 && l.pages[0].items[1].max == 40);
    assert(l.pages[0].items[1].sev[0] == 0 && l.pages[0].items[1].sev[1] == 25 && l.pages[0].items[1].sev[2] == 30);
    assert(l.pages[0].items[2].min == 0 && l.pages[0].items[2].max == 100 && isnan(l.pages[0].items[2].sev[0]));
    assert(parse_sections("[" SEC("{\"type\":\"glance\",\"show_state\":false,\"entities\":[\"light.a\","
                                  "{\"entity\":\"switch.b\",\"name\":\"Bee\",\"icon\":\"mdi:bee\"},\"sensor.c\",\"sensor.d\",\"sensor.e\"]}") "]", &l) == 1);
    assert(l.pages[0].count == LAYOUT_MAX_ROWS && l.pages[0].items[0].kind == LAYOUT_GLANCE);
    assert(!strcmp(l.pages[0].items[1].entity, "switch.b") && !strcmp(l.pages[0].items[1].name, "Bee") && !strcmp(l.pages[0].items[1].icon, "mdi:bee"));
    assert(l.pages[0].items[0].flags == LAYOUT_GLANCE_NO_STATE);
    assert(layout_sanitize(&l) && l.pages[0].items[0].flags == LAYOUT_GLANCE_NO_STATE);
}

static void test_weather(void)
{
    ha_layout_t l;
    assert(parse_sections("[" SEC("{\"type\":\"weather-forecast\",\"entity\":\"weather.home\",\"show_forecast\":false}") "]", &l) == 1);
    assert(l.pages[0].kind == LAYOUT_WEATHER && l.pages[0].count == 1 && !strcmp(l.pages[0].items[0].entity, "weather.home"));
    l.pages[0].items[0].flags = 0xFF;
    assert(layout_sanitize(&l) && l.pages[0].kind == LAYOUT_WEATHER && !l.pages[0].items[0].flags);
}

int main(void)
{
    test_picture();
    test_weather();
    test_icons();
    test_heading_icon();
    test_new_cards();
    test_clock();
    test_buttons();
    test_filter_kind();
    test_sanitize();
    test_fixture();
    test_name();
    test_name_template();
    test_hours();
    test_skips_unknown();
    test_no_heading();
    test_graph_first_wins();
    test_several_graphs();
    test_limits();
    test_bad_input();
    test_hash();
    test_filter();
    puts("all layout tests passed");
    return 0;
}
