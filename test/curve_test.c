/* Host tests for main/curve.c. Build/run: see test/run.sh */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "curve.h"

int main(void)
{
    float p[13];
    bool h[13] = {0};
    /* points at 2, 6, 10: 1, 3, 2 */
    for (int i = 0; i < 13; i++) p[i] = -99;
    p[2] = 1; p[6] = 3; p[10] = 2; h[2] = h[6] = h[10] = true;
    curve_fill(p, h, 13);
    assert(isnan(p[0]) && isnan(p[1]));                      /* before the first point */
    assert(p[2] == 1 && p[6] == 3 && p[10] == 2);            /* through every point */
    assert(p[11] == 2 && p[12] == 2);                        /* after the last: held */
    for (int i = 2; i < 6; i++) assert(p[i + 1] >= p[i]);    /* rising piece stays rising */
    for (int i = 6; i < 10; i++) assert(p[i + 1] <= p[i]);   /* falling piece stays falling */
    for (int i = 2; i <= 10; i++) assert(p[i] >= 1 && p[i] <= 3);   /* no overshoot */
    assert(p[4] > 1.5f && p[4] < 2.5f);                      /* smooth, not a step */
    /* flat neighbours stay flat; a single point is held */
    bool h2[5] = {1, 0, 0, 0, 1}; float q[5] = {4, 0, 0, 0, 4};
    curve_fill(q, h2, 5);
    for (int i = 0; i < 5; i++) assert(q[i] == 4);
    bool h3[4] = {0, 1, 0, 0}; float r[4] = {0, 7, 0, 0};
    curve_fill(r, h3, 4);
    assert(isnan(r[0]) && r[1] == 7 && r[3] == 7);
    bool h4[3] = {0}; float z[3];
    curve_fill(z, h4, 3);
    assert(isnan(z[0]) && isnan(z[2]));
    puts("curve_test: ok");
    return 0;
}
