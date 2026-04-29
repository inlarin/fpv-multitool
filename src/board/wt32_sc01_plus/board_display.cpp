#include "board_display.h"
#include "board_settings.h"
#include "lgfx_sc01_plus.h"   // confined to this TU -- never include from any header

struct BoardDisplay::Impl {
    LGFX_SC01Plus  lcd;
    uint8_t        rot = 0;
};

BoardDisplay::BoardDisplay()  : _impl(new Impl()) {}
BoardDisplay::~BoardDisplay() { delete _impl; }

// Run the LovyanGFX built-in 4-corner calibration with user-friendly
// instructions on screen. Stores the resulting 8x uint16_t matrix in
// NVS so subsequent boots skip this.
static void runInteractiveCalibrate(LGFX_SC01Plus &lcd) {
    lcd.fillScreen(TFT_BLACK);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setTextSize(2);
    lcd.setCursor(8, 8);
    lcd.println("Touch calibration");
    lcd.setCursor(8, 32);
    lcd.println("Tap the corner");
    lcd.setCursor(8, 52);
    lcd.println("markers as they");
    lcd.setCursor(8, 72);
    lcd.println("appear...");
    delay(800);

    uint16_t cal[8] = {0};
    lcd.calibrateTouch(cal, TFT_RED, TFT_BLACK, /* size = */ 30);
    BoardSettings::setTouchCalibrate(cal);

    lcd.fillScreen(TFT_BLACK);
    lcd.setCursor(8, 8);
    lcd.println("Calibration saved.");
    delay(400);
}

bool BoardDisplay::begin() {
    _impl->lcd.init();
    _impl->lcd.setBrightness(255);

    // Apply user rotation BEFORE calibration so the touch matrix learns
    // the right physical-to-user mapping for the saved orientation.
    setRotation(BoardSettings::rotation());

    // Touch calibration: load from NVS or run interactive.
    uint16_t cal[8];
    if (BoardSettings::getTouchCalibrate(cal)) {
        _impl->lcd.setTouchCalibrate(cal);
    } else {
        runInteractiveCalibrate(_impl->lcd);
    }

    return true;
}

void BoardDisplay::setRotation(uint8_t rot) {
    if (rot > 3) rot = 0;
    _impl->rot = rot;
    _impl->lcd.setRotation(rot);
    BoardSettings::setRotation(rot);
}

uint8_t BoardDisplay::rotation() const { return _impl->rot; }
int16_t BoardDisplay::width()  const   { return _impl->lcd.width(); }
int16_t BoardDisplay::height() const   { return _impl->lcd.height(); }

void BoardDisplay::setBrightness(uint8_t b) {
    _impl->lcd.setBrightness(b);
}

void BoardDisplay::pushPixels(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *px) {
    // pushImage (synchronous): canonical LVGL+LovyanGFX flush path
    // (see LVGL forum). Smart pixel converter + sync = no DMA race.
    // Buffer is already big-endian RGB565 from lv_draw_sw_rgb565_swap()
    // in the flush_cb.
    _impl->lcd.pushImage(x, y, w, h, px);
}

UiTouch BoardDisplay::readTouch() {
    UiTouch out{false, -1, -1};
    int32_t x = -1, y = -1;
    if (_impl->lcd.getTouch(&x, &y)) {
        out.pressed = true;
        out.x = (int16_t)x;
        out.y = (int16_t)y;
    }
    return out;
}

void BoardDisplay::recalibrateTouch() {
    BoardSettings::clearTouchCalibrate();
    runInteractiveCalibrate(_impl->lcd);
}
