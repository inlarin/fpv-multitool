#include "display.h"

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
