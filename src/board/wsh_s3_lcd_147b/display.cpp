#include "display.h"

#if defined(BOARD_WT32_SC01_PLUS)

// SC01 Plus uses LovyanGFX via BoardDisplay (src/board/wt32_sc01_plus/),
// and LVGL on top. The legacy Arduino_GFX-based menu/tester screens are
// not compiled into the SC01 Plus build (excluded via build_src_filter).
// We keep the Display:: namespace as a no-op stub so anything still
// referencing it links cleanly without a hard wall of #ifdefs.
void          Display::init()                       {}
Arduino_GFX*  Display::gfx()                        { return nullptr; }
void          Display::backlight(bool /*on*/)       {}
void          Display::setRotation(uint8_t /*r*/)   {}
uint8_t       Display::rotation()                   { return 0; }

#else

static Arduino_DataBus *s_bus = nullptr;
static Arduino_GFX *s_gfx = nullptr;

// LCD_BL on this board (and the diymore clone) drives the backlight FET
// gate through a low-side resistor; bare digitalWrite HIGH ends up dim
// in practice (the gate doesn't fully open at the GPIO pin's drive
// strength). Driving the same pin with LEDC PWM at 100 % duty pushes
// the FET fully open and gives the panel its rated brightness.
//
// Channel 0 / timer 0 is safe -- this board uses no other LEDC
// consumers (LovyanGFX is SC01-Plus only; ServoPWM acquires Port B's
// signal pin, not LCD_BL).
static const int BL_CHANNEL = 0;
static const int BL_FREQ_HZ = 5000;
static const int BL_RES_BITS = 8;     // 0..255 duty

void Display::init() {
    s_bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI);
    // Rotation 3 = 270 deg landscape (320x172). User feedback: rot=1
    // came up upside-down with USB-C on the left; rot=3 is right-side
    // up for that mount, so it's the more useful boot default. IMU
    // auto-rotate will pick a different rotation if the user holds
    // the board another way.
    //
    // 172x320 panel inside a 240x320 ST7789 controller -> 34px column
    // margin in portrait. Arduino_TFT.cpp::setRotation() routes the
    // four (col1,row1,col2,row2) constructor args differently per
    // rotation:
    //   rot 0: xStart = COL_OFFSET1, yStart = ROW_OFFSET1
    //   rot 1: xStart = ROW_OFFSET1, yStart = COL_OFFSET2
    //   rot 2: xStart = COL_OFFSET2, yStart = ROW_OFFSET2
    //   rot 3: xStart = ROW_OFFSET2, yStart = COL_OFFSET1
    // We want xStart=34/yStart=0 in portrait and xStart=0/yStart=34 in
    // landscape, which works out to all four offsets being symmetric:
    // COL_OFFSET1 = COL_OFFSET2 = 34, ROW_OFFSET1 = ROW_OFFSET2 = 0.
    s_gfx = new Arduino_ST7789(s_bus, LCD_RST, 3, true,
        LCD_WIDTH, LCD_HEIGHT, LCD_COL_OFFSET, 0, LCD_COL_OFFSET, 0);

    s_gfx->begin();

    // Configure LCD_BL as LEDC output, max duty by default.
    ledcAttachChannel(LCD_BL, BL_FREQ_HZ, BL_RES_BITS, BL_CHANNEL);
    ledcWrite(LCD_BL, 255);

    s_gfx->fillScreen(RGB565_BLACK);
}

Arduino_GFX* Display::gfx() {
    return s_gfx;
}

void Display::backlight(bool on) {
    ledcWrite(LCD_BL, on ? 255 : 0);
}

void Display::setRotation(uint8_t r) {
    if (s_gfx) s_gfx->setRotation(r);
}

uint8_t Display::rotation() {
    return s_gfx ? s_gfx->getRotation() : 0;
}

#endif  // BOARD_WT32_SC01_PLUS
