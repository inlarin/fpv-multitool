#include "display.h"

#if defined(BOARD_WT32_SC01_PLUS)

// SC01 Plus uses LovyanGFX via BoardDisplay (src/board/wt32_sc01_plus/),
// and LVGL on top. The legacy Arduino_GFX-based menu/tester screens are
// not compiled into the SC01 Plus build (excluded via build_src_filter).
// We keep the Display:: namespace as a no-op stub so anything still
// referencing it links cleanly without a hard wall of #ifdefs.
void          Display::init()                 {}
Arduino_GFX*  Display::gfx()                  { return nullptr; }
void          Display::backlight(bool /*on*/) {}

#else

static Arduino_DataBus *s_bus = nullptr;
static Arduino_GFX *s_gfx = nullptr;

void Display::init() {
    s_bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI);
    s_gfx = new Arduino_ST7789(s_bus, LCD_RST, 0, true,
        LCD_WIDTH, LCD_HEIGHT, LCD_COL_OFFSET, 0, LCD_COL_OFFSET, 0);

    s_gfx->begin();
    pinMode(LCD_BL, OUTPUT);
    backlight(true);
    s_gfx->fillScreen(RGB565_BLACK);
}

Arduino_GFX* Display::gfx() {
    return s_gfx;
}

void Display::backlight(bool on) {
    digitalWrite(LCD_BL, on ? HIGH : LOW);
}

#endif  // BOARD_WT32_SC01_PLUS
