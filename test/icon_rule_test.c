#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "icon_rule.h"

static const char *SENSOR =
    "{\"id\":3,\"type\":\"result\",\"success\":true,\"result\":{\"resources\":{\"sensor\":{"
    "\"_\":{\"default\":\"mdi:eye\"},"
    "\"battery\":{\"default\":\"mdi:battery-unknown\",\"range\":{\"0\":\"mdi:battery-alert\",\"10\":\"mdi:battery-10\","
    "\"80\":\"mdi:battery-80\",\"90\":\"mdi:battery-90\",\"100\":\"mdi:battery\"}},"
    "\"duration\":{\"default\":\"mdi:progress-clock\"}}}}}";
static const char *SWITCH =
    "{\"id\":4,\"type\":\"result\",\"result\":{\"resources\":{\"switch\":{\"_\":{\"default\":\"mdi:toggle-switch-variant\","
    "\"state\":{\"off\":\"mdi:toggle-switch-variant-off\"}},\"x\":{\"state\":{\"on\":\"y\"},\"default\":\"z\"}}}}}";
static const char *PING =
    "{\"id\":5,\"type\":\"result\",\"result\":{\"resources\":{\"ping\":{\"binary_sensor\":{\"loss\":{\"default\":\"wrong\"}},"
    "\"sensor\":{\"loss\":{\"default\":\"mdi:alert-circle-outline\"}}}}}}";

int main(void)
{
    char rule[256], icon[48];
    assert(icon_rule_from_icons(SENSOR, "sensor", NULL, "battery", rule, sizeof(rule)));
    icon_rule_apply(rule, "80", icon, sizeof(icon));   assert(!strcmp(icon, "mdi:battery-80"));
    icon_rule_apply(rule, "85.5", icon, sizeof(icon)); assert(!strcmp(icon, "mdi:battery-80"));
    icon_rule_apply(rule, "100", icon, sizeof(icon));  assert(!strcmp(icon, "mdi:battery"));
    icon_rule_apply(rule, "3", icon, sizeof(icon));    assert(!strcmp(icon, "mdi:battery-alert"));
    icon_rule_apply(rule, "unknown", icon, sizeof(icon)); assert(!strcmp(icon, "mdi:battery-unknown"));
    assert(icon_rule_from_icons(SENSOR, "sensor", NULL, "_", rule, sizeof(rule)));
    icon_rule_apply(rule, "5", icon, sizeof(icon));    assert(!strcmp(icon, "mdi:eye"));
    assert(!icon_rule_from_icons(SENSOR, "sensor", NULL, "power", rule, sizeof(rule)));

    assert(icon_rule_from_icons(SWITCH, "switch", NULL, "_", rule, sizeof(rule)));
    icon_rule_apply(rule, "off", icon, sizeof(icon));  assert(!strcmp(icon, "mdi:toggle-switch-variant-off"));
    icon_rule_apply(rule, "on", icon, sizeof(icon));   assert(!strcmp(icon, "mdi:toggle-switch-variant"));
    assert(icon_rule_from_icons(SWITCH, "switch", NULL, "x", rule, sizeof(rule)));   /* state before default */
    icon_rule_apply(rule, "on", icon, sizeof(icon));   assert(!strcmp(icon, "y"));
    icon_rule_apply(rule, "off", icon, sizeof(icon));  assert(!strcmp(icon, "z"));

    /* only the entity's own domain counts */
    assert(icon_rule_from_icons(PING, "ping", "sensor", "loss", rule, sizeof(rule)));
    icon_rule_apply(rule, "0.0", icon, sizeof(icon));  assert(!strcmp(icon, "mdi:alert-circle-outline"));
    puts("all icon rule tests passed");
    return 0;
}
