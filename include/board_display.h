#pragma once

#include <stdint.h>

// Single owner of the WT32-SC01 Plus LCD + touch hardware. LovyanGFX's
// built-in Touch_FT5x06 driver handles raw FT6336 reads, calibration,
// and rotation-aware coord mapping -- we never see raw coords above
// this layer. Calibration is loaded from NVS at begin(); if missing,
// we run an interactive calibrateTouch() and persist the result.
//
// THIS HEADER INTENTIONALLY DOES NOT INCLUDE LovyanGFX. Pulling in
// <LovyanGFX.hpp> drags in lgfx/v1/lv_font/color.h, which redefines
// LV_COLOR_FORMAT_* enums and conflicts with the real LVGL library
// when both end up in the same translation unit. The fix is to keep
// LovyanGFX strictly inside board_display.cpp -- this Pimpl layer
// gives every other TU a clean LVGL-friendly API.

struct UiTouch {
    bool    pressed;
    int16_t x;       // user-frame coords, current rotation applied (LovyanGFX maps internally)
    int16_t y;
};

class BoardDisplay {
public:
    BoardDisplay();
    ~BoardDisplay();

    // Init LCD + touch + load rotation from NVS. Must be called after
    // BoardSettings::begin(). If no touch calibration is stored, runs
    // an interactive 4-corner calibration and persists the result.
    // Returns true if everything came up.
    bool begin();

    // Set user-facing rotation 0..3. Persists to NVS, hardware-rotates
    // the LCD AND tells the touch driver to remap coords accordingly.
    // width()/height() change when rotating between portrait <-> landscape.
    void    setRotation(uint8_t rot);
    uint8_t rotation() const;

    int16_t width()  const;     // current rotation
    int16_t height() const;

    void    setBrightness(uint8_t b);

    // Push an already-rendered RGB565 buffer to (x, y) of size w*h.
    // Used by the LVGL flush callback.
    void    pushPixels(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *px);

    // Poll the touch panel via LovyanGFX. Returns user-frame coordinates
    // (already passed through the calibration matrix and current rotation).
    UiTouch readTouch();

    // Force a fresh interactive calibration -- wipes NVS, runs
    // calibrateTouch(), saves the new result. Call from a "Recalibrate"
    // settings menu item, or from a startup-key combo.
    void    recalibrateTouch();

    // Native panel dimensions (rotation-independent).
    static constexpr int16_t NATIVE_W = 320;
    static constexpr int16_t NATIVE_H = 480;

private:
    struct Impl;     // forward declaration; full def in board_display.cpp
    Impl *_impl;
};
