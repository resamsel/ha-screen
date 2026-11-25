#pragma once
/* IMU readings as Home Assistant sensors (issue #39). Pure helpers, host-testable. */

/* Raw QMI8658 scales: accelerometer 4 g, gyroscope 512 dps over a signed 16-bit range */
#define IMU_ACC_G(raw)   ((raw) * (4.0f / 32768.0f))
#define IMU_GYR_DPS(raw) ((raw) * (512.0f / 32768.0f))

/* How the board is held, from the raw accelerometer: "portrait", "portrait_flipped",
 * "landscape", "landscape_flipped", "flat" (screen up), "face_down", or "tilted" */
const char *imu_orientation(int ax, int ay, int az);

/* Angle in degrees between the screen's normal and straight up: 0 flat, 90 upright, 180 face down */
float imu_tilt(int ax, int ay, int az);
