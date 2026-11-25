#include "imu.h"
#include <math.h>
#include <stdlib.h>

/* Same axes as auto-rotate in main.c: -y up is portrait, +x up landscape_flipped (270).
 * z points out of the back of the board: lying screen up reads about -1 g (checked on a board). */
const char *imu_orientation(int ax, int ay, int az)
{
    int axa = abs(ax), aya = abs(ay), aza = abs(az);
    int inplane = axa > aya ? axa : aya;
    if (aza > 2 * inplane && aza > 4096) return az < 0 ? "flat" : "face_down";
    if (inplane < 4096 || aza > inplane) return "tilted";
    if (axa * 2 < aya * 3 && aya * 2 < axa * 3) return "tilted";   /* near a diagonal */
    if (aya > axa) return ay < 0 ? "portrait" : "portrait_flipped";
    return ax > 0 ? "landscape_flipped" : "landscape";
}

float imu_tilt(int ax, int ay, int az)
{
    float n = sqrtf((float)ax * ax + (float)ay * ay + (float)az * az);
    if (n < 1.0f) return 0.0f;
    float c = -az / n;                   /* screen normal = -z */
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    return acosf(c) * (180.0f / 3.14159265f);
}

bool imu_moved(const int prev_acc[3], const int acc[3], const int gyr[3])
{
    const int ACC_D = 1229, GYR_D = 2560;   /* 0.15 g, 40 deg/s */
    for (int i = 0; i < 3; i++)
        if (abs(acc[i] - prev_acc[i]) > ACC_D || abs(gyr[i]) > GYR_D) return true;
    return false;
}
