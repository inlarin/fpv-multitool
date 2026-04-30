// QMI8658 minimal accel-only driver. See imu.h for the public API.

#include "imu.h"

#include <Arduino.h>
#include <Wire.h>
#include "pin_config.h"

namespace {

constexpr uint8_t I2C_ADDR    = 0x6B;
constexpr uint32_t I2C_FREQ   = 400000;

// QMI8658 register map (subset).
constexpr uint8_t REG_WHO_AM_I = 0x00;     // -> 0x05
constexpr uint8_t REG_CTRL1    = 0x02;     // serial interface + sensor enable
constexpr uint8_t REG_CTRL2    = 0x03;     // accel: full-scale + ODR
constexpr uint8_t REG_CTRL7    = 0x08;     // enable accel / gyro
constexpr uint8_t REG_AX_L     = 0x35;     // accel data (6 bytes auto-inc)

constexpr uint8_t WHO_AM_I_VAL = 0x05;

bool s_ready = false;

bool writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

int readBytes(uint8_t reg, uint8_t *buf, size_t n) {
    Wire.beginTransmission(I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;   // no STOP -> repeated START
    size_t got = Wire.requestFrom((uint8_t)I2C_ADDR, (uint8_t)n);
    for (size_t i = 0; i < got && i < n; i++) buf[i] = Wire.read();
    return (int)got;
}

} // namespace

bool IMU::init() {
    Wire.begin(I2C_SDA, I2C_SCL, I2C_FREQ);

    // WHO_AM_I sanity check.
    uint8_t who = 0;
    if (readBytes(REG_WHO_AM_I, &who, 1) != 1 || who != WHO_AM_I_VAL) {
        Serial.printf("[IMU] no QMI8658 (WHO_AM_I=0x%02X, expected 0x%02X)\n",
                      who, WHO_AM_I_VAL);
        s_ready = false;
        return false;
    }

    // CTRL1: address auto-increment ON (bit6=1), big-endian OFF (bit5=0),
    // serial interface + sensor enabled (default). 0x60.
    if (!writeReg(REG_CTRL1, 0x60)) goto fail;
    // CTRL2: accel full-scale = +-2g (aFS=000), ODR ~58 Hz (aODR=0101).
    // 0x05 -- plenty for orientation polling at 1 Hz, low power.
    if (!writeReg(REG_CTRL2, 0x05)) goto fail;
    // CTRL7: enable accel only (gyro left off to save power).
    if (!writeReg(REG_CTRL7, 0x01)) goto fail;

    Serial.println("[IMU] QMI8658 ready (accel-only, +-2g, 58Hz)");
    s_ready = true;
    return true;

fail:
    Serial.println("[IMU] config writes failed");
    s_ready = false;
    return false;
}

bool IMU::isReady() { return s_ready; }

bool IMU::readAccel(int16_t &x, int16_t &y, int16_t &z) {
    if (!s_ready) { x = y = z = 0; return false; }
    uint8_t buf[6] = {0};
    if (readBytes(REG_AX_L, buf, 6) != 6) { x = y = z = 0; return false; }
    x = (int16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
    y = (int16_t)((uint16_t)buf[2] | ((uint16_t)buf[3] << 8));
    z = (int16_t)((uint16_t)buf[4] | ((uint16_t)buf[5] << 8));
    return true;
}

uint8_t IMU::detectOrientation() {
    int16_t x, y, z;
    if (!readAccel(x, y, z)) return 1;   // default landscape

    // We pick the rotation by which axis carries gravity.
    // Threshold: 0.5g = 8192 LSB at +-2g range.
    constexpr int16_t T = 8192;

    int ax = abs(x), ay = abs(y);

    // Empirical axis mapping for Waveshare ESP32-S3-LCD-1.47B:
    //   USB-C on bottom edge in portrait orientation.
    //   Board flat on table, screen up: gravity points -Z (z ~ -16384).
    //   Board upright, USB-C down (portrait normal):  +Y is down  (rot 0).
    //   Board upright, USB-C up   (portrait flipped):  -Y is down  (rot 2).
    //   Board on its right side  (USB-C on left, landscape): +X down (rot 1).
    //   Board on its left side   (USB-C on right):           -X down (rot 3).
    //
    // If the empirical mapping turns out wrong on a particular unit
    // (different physical mount, board flipped on housing), tweak the
    // four returns below -- they're the only board-specific bit.
    if (ay >= T && ay >= ax) {
        return (y > 0) ? 0 : 2;
    }
    if (ax >= T && ax > ay) {
        return (x > 0) ? 1 : 3;
    }

    // Z dominant (board flat, no gravity in plane): keep last-known
    // by returning 1 as a benign default.
    return 1;
}
