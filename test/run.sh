#!/bin/sh
# Host tests for the layout parser; run inside the IDF image:
#   docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.3.2 sh test/run.sh
set -e
J=/opt/esp/idf/components/json/cJSON
gcc -std=c11 -Wall -Werror -DLAYOUT_HOST -I main -I $J main/layout.c $J/cJSON.c test/layout_test.c -lm -o /tmp/layout_test
/tmp/layout_test
gcc -std=c11 -Wall -Werror -I main main/names.c test/names_test.c -o /tmp/names_test
/tmp/names_test
