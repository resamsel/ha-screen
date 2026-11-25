/* Smooth line through sparse graph points (statistics graphs, like HA's). Pure, host-tested. */
#pragma once
#include <stdbool.h>

/* pts[i] is a point where has[i]; the others get a monotone cubic curve (Fritsch-Carlson:
 * through every point, no overshoot beyond its neighbours). Before the first point: NAN;
 * after the last: the last value. */
#define CURVE_MAX 256
void curve_fill(float *pts, const bool *has, int n);   /* n <= CURVE_MAX */
