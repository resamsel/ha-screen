/* Host tests for main/live.c. Build/run: see test/run.sh */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "live.h"

static const char *IDS[] = { "sensor.temp", "light.desk", "weather.home", "input_number.bri" };

static void test_initial_states(void)
{
    live_t *l = live_new(IDS, 4);
    int ch[8];
    const char *a = "{\"id\":5,\"type\":\"event\",\"event\":{\"a\":{"
        "\"sensor.temp\":{\"s\":\"21.53\",\"a\":{\"unit_of_measurement\":\"\xc2\xb0""C\",\"friendly_name\":\"Kitchen \\\"main\\\"\"},\"c\":\"x\",\"lc\":1.0},"
        "\"sensor.other\":{\"s\":\"1\",\"a\":{}},"
        "\"weather.home\":{\"s\":\"sunny\",\"a\":{\"temperature\":12.6,\"humidity\":60,\"temperature_unit\":\"\xc2\xb0""C\"}}}}}";
    int n = live_apply(l, a, ch, 8);
    assert(n == 2 && ch[0] == 0 && ch[1] == 2);          /* not tracked: ignored */
    char j[256];
    assert(live_json(l, 0, j, sizeof(j)));
    assert(strstr(j, "\"state\":\"21.53\""));
    assert(strstr(j, "\"unit_of_measurement\":\"\xc2\xb0""C\""));
    assert(strstr(j, "\"friendly_name\":\"Kitchen 'main'\""));   /* quotes can't break the JSON */
    assert(live_json(l, 2, j, sizeof(j)));
    assert(strstr(j, "\"temperature\":12.6") && strstr(j, "\"humidity\":60"));
    assert(!live_json(l, 1, j, sizeof(j)));              /* nothing received yet */
    assert(live_apply(l, a, ch, 8) == 0);                 /* same again: no change */
    live_free(l);
}

static void test_changes(void)
{
    live_t *l = live_new(IDS, 4);
    int ch[8];
    live_apply(l, "{\"type\":\"event\",\"event\":{\"a\":{\"light.desk\":{\"s\":\"off\",\"a\":{\"friendly_name\":\"Desk\",\"icon\":\"mdi:lamp\"}}}}}", ch, 8);
    /* state change only: attributes kept */
    assert(live_apply(l, "{\"type\":\"event\",\"event\":{\"c\":{\"light.desk\":{\"+\":{\"s\":\"on\",\"lc\":2.0}}}}}", ch, 8) == 1);
    char j[256];
    live_json(l, 1, j, sizeof(j));
    assert(strstr(j, "\"state\":\"on\"") && strstr(j, "\"friendly_name\":\"Desk\"") && strstr(j, "\"icon\":\"mdi:lamp\""));
    /* only a timestamp: no change reported */
    assert(live_apply(l, "{\"type\":\"event\",\"event\":{\"c\":{\"light.desk\":{\"+\":{\"lu\":3.0}}}}}", ch, 8) == 0);
    /* attribute removed */
    assert(live_apply(l, "{\"type\":\"event\",\"event\":{\"c\":{\"light.desk\":{\"-\":{\"a\":[\"icon\"]}}}}}", ch, 8) == 1);
    live_json(l, 1, j, sizeof(j));
    assert(!strstr(j, "icon"));
    /* entity removed: unavailable */
    assert(live_apply(l, "{\"type\":\"event\",\"event\":{\"r\":[\"light.desk\"]}}", ch, 8) == 1);
    live_json(l, 1, j, sizeof(j));
    assert(strstr(j, "\"state\":\"unavailable\""));
    live_free(l);
}

static void test_numbers_and_limits(void)
{
    live_t *l = live_new(IDS, 4);
    int ch[1];
    live_apply(l, "{\"type\":\"event\",\"event\":{\"a\":{"
        "\"input_number.bri\":{\"s\":\"42.0\",\"a\":{\"min\":0,\"max\":100}},"
        "\"sensor.temp\":{\"s\":\"1\",\"a\":{}}}}}", ch, 1);
    char j[256];
    live_json(l, 3, j, sizeof(j));
    assert(strstr(j, "\"min\":0") && strstr(j, "\"max\":100"));
    assert(!live_json(l, 3, j, 20));                      /* too small: false, not cut JSON */
    live_free(l);
}

static void test_bad_input(void)
{
    live_t *l = live_new(IDS, 4);
    int ch[4];
    assert(live_apply(l, "not json", ch, 4) == 0);
    assert(live_apply(l, NULL, ch, 4) == 0);
    assert(live_apply(l, "{\"id\":1,\"type\":\"result\",\"success\":true,\"result\":null}", ch, 4) == 0);
    assert(live_apply(NULL, "{}", ch, 4) == 0);
    assert(live_find(l, "sensor.temp") == 0 && live_find(l, "nope") == -1 && live_count(l) == 4);
    live_free(l);
}

/* climate attributes for the thermostat card */
static void test_climate(void)
{
    const char *ids[] = { "climate.x" };
    live_t *l = live_new(ids, 1);
    int ch[4];
    assert(live_apply(l, "{\"event\":{\"a\":{\"climate.x\":{\"s\":\"heat\",\"a\":{\"current_temperature\":20.7,"
                         "\"temperature\":21.5,\"min_temp\":15,\"max_temp\":28,\"target_temp_step\":0.5,\"hvac_action\":\"heating\"}}}}}", ch, 4) == 1);
    char j[512];
    assert(live_json(l, 0, j, sizeof(j)));
    assert(strstr(j, "\"current_temperature\":20.7") && strstr(j, "\"temperature\":21.5") && strstr(j, "\"min_temp\":15"));
    assert(strstr(j, "\"max_temp\":28") && strstr(j, "\"target_temp_step\":0.5") && strstr(j, "\"hvac_action\":\"heating\""));
    assert(live_apply(l, "{\"event\":{\"c\":{\"climate.x\":{\"+\":{\"a\":{\"media_title\":\"Song \\\"A\\\"\",\"media_artist\":\"Band\",\"brightness\":153}}}}}}", ch, 4) == 1);
    assert(live_json(l, 0, j, sizeof(j)));
    assert(strstr(j, "\"media_title\":\"Song 'A'\"") && strstr(j, "\"media_artist\":\"Band\"") && strstr(j, "\"brightness\":153"));
    live_free(l);
}

int main(void)
{
    test_climate();
    test_initial_states();
    test_changes();
    test_numbers_and_limits();
    test_bad_input();
    puts("all live tests passed");
    return 0;
}
