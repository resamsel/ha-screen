#include <stdio.h>
#include <string.h>
#include <math.h>
#include "imu.h"

static int fails;
#define EQS(a, b) do { if (strcmp((a), (b))) { printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, (a), (b)); fails++; } } while (0)
#define NEAR(a, b) do { if (fabsf((a) - (b)) > 0.5f) { printf("FAIL %s:%d: %f != %f\n", __FILE__, __LINE__, (double)(a), (double)(b)); fails++; } } while (0)

int main(void)
{
    const int G = 8192;   /* 1 g at the 4 g range */
    EQS(imu_orientation(0, -G, 0), "portrait");
    EQS(imu_orientation(0, G, 0), "portrait_flipped");
    EQS(imu_orientation(-G, 0, 0), "landscape");
    EQS(imu_orientation(G, 0, 0), "landscape_flipped");
    EQS(imu_orientation(100, 200, -G), "flat");   /* screen up: z reads -1 g */
    EQS(imu_orientation(100, 200, G), "face_down");
    EQS(imu_orientation(G / 2, -G / 2, 0), "tilted");      /* diagonal */
    EQS(imu_orientation(0, -G / 2, G * 3 / 4), "tilted");  /* half way to flat */
    EQS(imu_orientation(0, 0, 0), "tilted");
    NEAR(imu_tilt(0, 0, -G), 0.0f);
    NEAR(imu_tilt(0, -G, 0), 90.0f);
    NEAR(imu_tilt(0, 0, G), 180.0f);
    NEAR(imu_tilt(0, -G, -G), 45.0f);
    NEAR(IMU_ACC_G(G), 1.0f);
    NEAR(IMU_GYR_DPS(6400), 100.0f);
    if (fails) return 1;
    printf("imu ok\n");
    return 0;
}
