#pragma once
#include <stdint.h>

// Onboard QMI8658 6-axis IMU (Waveshare ESP32-S3-LCD-1.47B).
// I2C addr 0x6B on Wire0 (SDA=48, SCL=47 -- not on user pin headers).
//
// We only use the accelerometer for orientation detection -- the
// gyroscope is left disabled to save power.
//
// Use:
//   IMU::init() once in setup() (after Wire0 is available).
//   IMU::readAccel(...) any time after a successful init().
//   IMU::detectOrientation() returns the LCD rotation that makes
//                            "up" match physical "up" right now.

namespace IMU {

// Initialise QMI8658. Returns true if WHO_AM_I matched and config
// writes succeeded. After return, accelerometer streams in the
// background and readAccel() can be called any time.
bool init();

// Last init() outcome (cached). false if no IMU is responding -- in
// that case readAccel returns zero-vector and detectOrientation
// returns 1 (default landscape) so the caller still gets a usable
// answer.
bool isReady();

// Read raw accel registers. Units: signed 16-bit, full-scale +-2g
// means ~16384 LSB / g (gravity ~ 16384 on whichever axis is "down").
// Returns false if no IMU.
bool readAccel(int16_t &x, int16_t &y, int16_t &z);

// Map the current gravity direction to an Arduino_GFX rotation:
//   0 = portrait normal     (board long-axis vertical, USB on bottom)
//   1 = landscape (90 CW)   (board long-axis horizontal, USB on left)
//   2 = portrait flipped    (USB on top)
//   3 = landscape (90 CCW)  (USB on right)
//
// Hardware axis convention is verified empirically -- see imu.cpp
// for the mapping used. Returns 1 if no IMU / readAccel fails.
uint8_t detectOrientation();

} // namespace IMU
