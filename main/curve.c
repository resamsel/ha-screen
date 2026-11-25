#include "curve.h"
#include <math.h>

void curve_fill(float *pts, const bool *has, int n)
{
    static int idx[CURVE_MAX];            /* the known points (static: the HA task's stack is small) */
    static float d[CURVE_MAX], t[CURVE_MAX];   /* secant slopes, tangents (per bucket) */
    int m = 0;
    if (n > CURVE_MAX) n = CURVE_MAX;
    for (int i = 0; i < n; i++) if (has[i]) idx[m++] = i;
    if (!m) { for (int i = 0; i < n; i++) pts[i] = NAN; return; }
    for (int i = 0; i < idx[0]; i++) pts[i] = NAN;
    for (int i = idx[m - 1] + 1; i < n; i++) pts[i] = pts[idx[m - 1]];
    if (m < 2) return;
    for (int k = 0; k + 1 < m; k++) d[k] = (pts[idx[k + 1]] - pts[idx[k]]) / (float)(idx[k + 1] - idx[k]);
    t[0] = d[0];
    t[m - 1] = d[m - 2];
    for (int k = 1; k + 1 < m; k++) t[k] = d[k - 1] * d[k] <= 0 ? 0 : (d[k - 1] + d[k]) / 2;
    for (int k = 0; k + 1 < m; k++) {       /* keep each piece monotone */
        if (d[k] == 0) { t[k] = t[k + 1] = 0; continue; }
        float a = t[k] / d[k], b = t[k + 1] / d[k], s = a * a + b * b;
        if (s > 9) { float r = 3 / sqrtf(s); t[k] = r * a * d[k]; t[k + 1] = r * b * d[k]; }
    }
    for (int k = 0; k + 1 < m; k++) {
        int x0 = idx[k], x1 = idx[k + 1];
        float h = (float)(x1 - x0), y0 = pts[x0], y1 = pts[x1];
        for (int x = x0 + 1; x < x1; x++) {
            float u = (x - x0) / h, u2 = u * u, u3 = u2 * u;
            pts[x] = (2 * u3 - 3 * u2 + 1) * y0 + (u3 - 2 * u2 + u) * h * t[k]
                   + (-2 * u3 + 3 * u2) * y1 + (u3 - u2) * h * t[k + 1];
        }
    }
}
