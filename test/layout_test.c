/* Host tests for main/layout.c. Build/run: see test/run.sh */
#include <assert.h>
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
    assert(l.pages[2].hours == 24);
    assert(!strcmp(l.pages[4].title, "New section"));
    assert(!strcmp(l.pages[4].items[0].entity, "sensor.fritz_box_7590_ax_download_geschwindigkeit"));
    free(j);
}

static void test_name(void)
{
    ha_layout_t l;
    assert(parse_sections("[" SEC("{\"type\":\"tile\",\"entity\":\"sensor.a\",\"name\":{\"type\":\"device\"}}" ","
                                  "{\"type\":\"tile\",\"entity\":\"sensor.b\",\"name\":\"Ping\"}") "]", &l) == 1);
    assert(!strcmp(l.pages[0].items[0].name, ""));
    assert(!strcmp(l.pages[0].items[1].name, "Ping"));
}

static void test_hours(void)
{
    ha_layout_t l;
    assert(parse_sections("[" SEC("{\"type\":\"sensor\",\"graph\":\"line\",\"entity\":\"sensor.a\",\"hours_to_show\":6}")
                          "," SEC("{\"type\":\"sensor\",\"graph\":\"line\",\"entity\":\"sensor.a\",\"hours_to_show\":0}")
                          "," SEC("{\"type\":\"sensor\",\"graph\":\"line\",\"entity\":\"sensor.a\",\"hours_to_show\":\"x\"}")
                          "," SEC("{\"type\":\"sensor\",\"graph\":\"line\",\"entity\":\"sensor.a\",\"hours_to_show\":500}") "]", &l) == 4);
    assert(l.pages[0].hours == 6);
    assert(l.pages[1].hours == 24);
    assert(l.pages[2].hours == 24);
    assert(l.pages[3].hours == 168);
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
    assert(l.pages[0].count == 1);
    assert(!strcmp(l.pages[0].items[0].entity, "sensor.g"));
    assert(parse_sections("[" SEC("{\"type\":\"sensor\",\"entity\":\"sensor.s\"}") "]", &l) == 0);
}

static void test_limits(void)
{
    ha_layout_t l;
    const char *s = SEC(TILE("sensor.a"));
    char buf[4096] = "[";
    for (int i = 0; i < 10; i++) { strcat(buf, s); if (i < 9) strcat(buf, ","); }
    strcat(buf, "]");
    assert(parse_sections(buf, &l) == 8);
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

static bool keep_not_x(const char *entity, void *ctx)
{
    (void)ctx;
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
    assert(layout_sanitize(&l));
    l.pages[0].hours = 0;
    l.pages[1].kind = 7;
    assert(layout_sanitize(&l));
    assert(l.pages[0].hours == LAYOUT_HOURS_DEFAULT);
    assert(l.pages[1].kind == LAYOUT_VALUES);
    l.pages[0].hours = 9999;
    assert(layout_sanitize(&l) && l.pages[0].hours == LAYOUT_HOURS_MAX);
    l.pages[1].count = 9;
    assert(!layout_sanitize(&l) && l.count == 0);
    l.count = 42;
    assert(!layout_sanitize(&l));
}

int main(void)
{
    test_sanitize();
    test_fixture();
    test_name();
    test_hours();
    test_skips_unknown();
    test_no_heading();
    test_graph_first_wins();
    test_limits();
    test_bad_input();
    test_hash();
    test_filter();
    puts("all layout tests passed");
    return 0;
}
