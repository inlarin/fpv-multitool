// WT32-SC01 Plus — Sprint 32 step 2 LCD sanity sketch.
//
// Built ONLY by [env:wt32_sc01_plus_lcd]. Drives the ST7796 320x480 panel
// over the 8-bit 8080 parallel bus via LovyanGFX. Goal: confirm pin map +
// init sequence + backlight polarity before bolting on LVGL and touch.
//
// Expected sequence on power-up:
//   1. Backlight comes on (full brightness)
//   2. Color cycle: RED -> GREEN -> BLUE -> WHITE -> BLACK, ~700 ms each,
//      with the color name printed in opposite color (sanity for text)
//   3. Final test pattern:
//        - "WT32-SC01 Plus" + resolution + "LovyanGFX OK" in white
//        - 1px green border around the entire panel
//        - 40px grid in dark gray
//        - 4 corner markers: red(TL) green(TR) blue(BL) yellow(BR)
//          -> lets us verify orientation (TL = "top-left" near USB-C? or far?)
//   4. Loop: prints "alive N" once per second over USB CDC

#include <Arduino.h>
#include "lgfx_sc01_plus.h"

static LGFX_SC01Plus lcd;

static void colorPhase(uint16_t color, const char *name) {
    lcd.fillScreen(color);
    // Pick a contrasting text color: white on dark colors, black on white
    uint16_t text_color = (color == TFT_WHITE || color == TFT_YELLOW) ? TFT_BLACK : TFT_WHITE;
    lcd.setTextColor(text_color);
    lcd.setCursor(12, 12);
    lcd.setTextSize(4);
    lcd.print(name);
    Serial.printf("  fill: %s\n", name);
    delay(700);
}

static void drawTestPattern() {
    lcd.fillScreen(TFT_BLACK);

    // --- Header text ---
    lcd.setTextColor(TFT_WHITE);
    lcd.setTextSize(2);
    lcd.setCursor(8, 8);
    lcd.println("WT32-SC01 Plus");
    lcd.setCursor(8, 30);
    lcd.printf("LCD: %dx%d\n", (int)lcd.width(), (int)lcd.height());
    lcd.setCursor(8, 52);
    lcd.println("LovyanGFX OK");

    // --- 40px grid (skips the header band) ---
    for (int x = 40; x < lcd.width(); x += 40) {
        lcd.drawLine(x, 80, x, lcd.height() - 1, TFT_DARKGREY);
    }
    for (int y = 80; y < lcd.height(); y += 40) {
        lcd.drawLine(0, y, lcd.width() - 1, y, TFT_DARKGREY);
    }

    // --- 1px border ---
    lcd.drawRect(0, 0, lcd.width(), lcd.height(), TFT_GREEN);

    // --- Corner orientation markers ---
    //   red    = top-left  (near 0,0)
    //   green  = top-right
    //   blue   = bottom-left
    //   yellow = bottom-right
    const int r = 8;
    lcd.fillCircle(r + 2,                  r + 2,                   r, TFT_RED);
    lcd.fillCircle(lcd.width() - r - 2,    r + 2,                   r, TFT_GREEN);
    lcd.fillCircle(r + 2,                  lcd.height() - r - 2,    r, TFT_BLUE);
    lcd.fillCircle(lcd.width() - r - 2,    lcd.height() - r - 2,    r, TFT_YELLOW);

    Serial.println("LCD sanity: test pattern drawn");
}

void setup() {
    Serial.begin(115200);
    // Wait briefly for USB CDC to enumerate; don't block forever.
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { delay(10); }

    Serial.println();
    Serial.println(F("================ WT32-SC01 Plus LCD sanity ================"));
    Serial.println(F("init LovyanGFX..."));

    lcd.init();
    lcd.setRotation(0);          // portrait 320x480
    lcd.setBrightness(255);      // PWM duty 100%

    Serial.printf("  panel: %dx%d, color depth: %d bpp\n",
                  (int)lcd.width(), (int)lcd.height(), (int)lcd.getColorDepth());
    Serial.println(F("starting color cycle..."));

    colorPhase(TFT_RED,   "RED");
    colorPhase(TFT_GREEN, "GREEN");
    colorPhase(TFT_BLUE,  "BLUE");
    colorPhase(TFT_WHITE, "WHITE");
    colorPhase(TFT_BLACK, "BLACK");

    drawTestPattern();
    Serial.println(F("==========================================================="));
}

void loop() {
    // Heartbeat over USB so we can confirm the board hasn't crashed even
    // after the LCD is sitting idle.
    static uint32_t last = 0;
    static uint32_t n = 0;
    if (millis() - last >= 1000) {
        last = millis();
        Serial.printf("alive %lu  (free heap %u)\n",
                      (unsigned long)++n, (unsigned)ESP.getFreeHeap());
    }
}
