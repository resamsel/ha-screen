#!/bin/sh
# Host tests for the layout parser; run inside the IDF image:
#   docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.3.2 sh test/run.sh
set -e
J=/opt/esp/idf/components/json/cJSON
gcc -std=c11 -Wall -Werror -DLAYOUT_HOST -I main -I $J main/layout.c $J/cJSON.c test/layout_test.c -lm -o /tmp/layout_test
/tmp/layout_test
gcc -std=c11 -Wall -Werror -I main main/names.c main/tz.c test/names_test.c -o /tmp/names_test
/tmp/names_test
gcc -std=c11 -Wall -Werror -I main main/icon_rule.c test/icon_rule_test.c -o /tmp/icon_rule_test
/tmp/icon_rule_test
gcc -std=c11 -Wall -Werror -I main -I $J main/live.c $J/cJSON.c test/live_test.c -lm -o /tmp/live_test
/tmp/live_test
gcc -std=c11 -Wall -Werror -I main main/notify.c test/notify_test.c -o /tmp/notify_test
/tmp/notify_test
gcc -std=c11 -Wall -Werror -I main main/screen.c test/screen_test.c -o /tmp/screen_test
/tmp/screen_test
gcc -std=gnu11 -Wall -Werror -I main main/cal_parse.c test/cal_parse_test.c -o /tmp/cal_parse_test
TZ=UTC /tmp/cal_parse_test
