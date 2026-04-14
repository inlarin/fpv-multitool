#include "motor_tester.h"
#include <Arduino.h>
#include "pin_config.h"
#include "ui/display.h"
#include "ui/button.h"
#include "dshot.h"
#include "wdt.h"

static const int MOTOR_PIN = SIGNAL_OUT;

// DShot special commands
static const uint8_t DSHOT_CMD_BEEP1 = 1;
static const uint8_t DSHOT_CMD_BEEP2 = 2;
static const uint8_t DSHOT_CMD_BEEP3 = 3;
static const uint8_t DSHOT_CMD_BEEP4 = 4;
static const uint8_t DSHOT_CMD_BEEP5 = 5;
static const uint8_t DSHOT_CMD_SPIN_DIR_1 = 7;
static const uint8_t DSHOT_CMD_SPIN_DIR_2 = 8;
static const uint8_t DSHOT_CMD_3D_MODE_OFF = 9;
static const uint8_t DSHOT_CMD_3D_MODE_ON = 10;

enum MotorPage { MPAGE_CONTROL, MPAGE_SETUP, MPAGE_COUNT };

static DShotSpeed s_dshotSpeed = DSHOT300;
static uint16_t s_throttle = 0;     // 0-2047
static bool s_armed = false;
static bool s_safetyLock = true;     // Prevent accidental throttle
static int s_throttleStep = 50;

static const char* speedName(DShotSpeed s) {
    switch(s) {
        case DSHOT150: return "DShot150";
        case DSHOT300: return "DShot300";
        case DSHOT600: return "DShot600";
        default: return "?";
    }
}

// Persistent state bar: always visible at top, color-coded
static void drawStateBar() {
    auto *g = Display::gfx();
    // Color-coded state bar across top
    uint16_t bg = RGB565_MAROON;  // disarmed = dark red
    const char* state = "DISARMED";
    if (s_armed && s_safetyLock) {
        bg = RGB565_OLIVE;        // armed+locked = dark yellow
        state = "ARMED (LOCKED)";
    } else if (s_armed && !s_safetyLock) {
        bg = RGB565_DARKGREEN;    // live = dark green
        state = "LIVE — motor may spin";
    }
    g->fillRect(0, 0, LCD_WIDTH, 22, bg);
    g->setTextSize(1);
    g->setTextColor(RGB565_WHITE);
    g->setCursor(5, 7);
    g->print(state);
    // Throttle % in top-right
    int pct = s_throttle * 100 / 2047;
    char buf[10];
    snprintf(buf, sizeof(buf), "%3d%%", pct);
    g->setCursor(LCD_WIDTH - 30, 7);
    g->print(buf);
}

static void drawControl() {
    auto *g = Display::gfx();
    g->fillScreen(RGB565_BLACK);

    drawStateBar();

    // Title row (below state bar)
    g->setTextSize(1);
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(5, 28);
    g->printf("%s  GPIO%d", speedName(s_dshotSpeed), MOTOR_PIN);

    // Large "what to press next" hint
    g->setTextSize(1);
    g->setCursor(5, 42);
    if (!s_armed) {
        g->setTextColor(RGB565_CYAN);
        g->print("Hold = setup / arm");
    } else if (s_safetyLock) {
        g->setTextColor(RGB565_YELLOW);
        g->print("Click = UNLOCK throttle");
    } else {
        g->setTextColor(RGB565_GREEN);
        g->print("Clk/Dbl = throttle +/-");
    }

    // Throttle value
    g->setTextSize(3);
    g->setTextColor(RGB565_WHITE);
    g->setCursor(20, 80);
    g->printf("%4d", s_throttle);

    // Percentage
    int pct = s_throttle * 100 / 2047;
    g->setTextSize(2);
    g->setTextColor(RGB565_CYAN);
    g->setCursor(60, 115);
    g->printf("%3d%%", pct);

    // Throttle bar
    int barY = 145;
    int barH = 16;
    int barW = LCD_WIDTH - 20;
    g->drawRect(9, barY - 1, barW + 2, barH + 2, RGB565_DARKGREY);

    int fillW = s_throttle * barW / 2047;
    uint16_t barColor = RGB565_GREEN;
    if (pct > 50) barColor = RGB565_YELLOW;
    if (pct > 80) barColor = RGB565_RED;
    if (fillW > 0) g->fillRect(10, barY, fillW, barH, barColor);

    // Scale
    g->setTextSize(1);
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(5, barY + barH + 4);
    g->print("0");
    g->setCursor(70, barY + barH + 4);
    g->print("1024");
    g->setCursor(140, barY + barH + 4);
    g->print("2047");

    // Safety warning
    if (s_throttle > 0 && s_armed) {
        g->setTextSize(1);
        g->setTextColor(RGB565_RED);
        g->setCursor(5, 195);
        g->print("!! MOTOR SPINNING !!");
    }

    // Step size
    g->setTextSize(1);
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(5, 210);
    g->printf("Step: %d", s_throttleStep);

    // Footer
    g->setCursor(3, LCD_HEIGHT - 24);
    if (!s_armed) {
        g->print("Hld=setup/arm");
    } else if (s_safetyLock) {
        g->print("Clk=unlock  Hld=disarm");
    } else {
        g->print("Clk=+  Dbl=-  Hld=setup");
    }
    g->setCursor(3, LCD_HEIGHT - 12);
    g->setTextColor(RGB565_RED);
    g->print("! REMOVE PROPELLERS !");
}

static void updateThrottleDisplay() {
    auto *g = Display::gfx();

    // Redraw state bar (pct in header changes too)
    drawStateBar();

    // Throttle number
    g->fillRect(20, 80, 140, 28, RGB565_BLACK);
    g->setTextSize(3);
    g->setTextColor(RGB565_WHITE);
    g->setCursor(20, 80);
    g->printf("%4d", s_throttle);

    // Percentage
    int pct = s_throttle * 100 / 2047;
    g->fillRect(60, 115, 80, 20, RGB565_BLACK);
    g->setTextSize(2);
    g->setTextColor(RGB565_CYAN);
    g->setCursor(60, 115);
    g->printf("%3d%%", pct);

    // Bar
    int barY = 145;
    int barH = 16;
    int barW = LCD_WIDTH - 20;
    g->fillRect(10, barY, barW, barH, RGB565_BLACK);
    int fillW = s_throttle * barW / 2047;
    uint16_t barColor = RGB565_GREEN;
    if (pct > 50) barColor = RGB565_YELLOW;
    if (pct > 80) barColor = RGB565_RED;
    if (fillW > 0) g->fillRect(10, barY, fillW, barH, barColor);
}

// Setup menu: speed, arm, commands, exit
enum SetupItem { SI_ARM, SI_SPEED, SI_BEEP, SI_STEP, SI_BACK, SI_COUNT };

static bool runSetup() {
    int sel = 0;
    auto *g = Display::gfx();

    while (true) {
        g->fillScreen(RGB565_BLACK);
        g->setTextSize(2);
        g->setTextColor(RGB565_ORANGE);
        g->setCursor(10, 6);
        g->println("Motor Setup");
        g->drawFastHLine(0, 28, LCD_WIDTH, RGB565_DARKGREY);

        const char* labels[SI_COUNT];
        labels[SI_ARM] = s_armed ? "Disarm" : "Arm ESC";
        labels[SI_SPEED] = speedName(s_dshotSpeed);
        labels[SI_BEEP] = "Beep ESC";
        labels[SI_STEP] = "Step size";
        labels[SI_BACK] = "<< Menu";

        for (int i = 0; i < SI_COUNT; i++) {
            int y = 35 + i * 32;
            if (i == sel) {
                uint16_t bg = (i == SI_BACK) ? RGB565_MAROON : RGB565_NAVY;
                g->fillRoundRect(6, y, LCD_WIDTH - 12, 26, 4, bg);
                g->setTextColor(RGB565_WHITE);
            } else {
                g->setTextColor(RGB565_DARKGREY);
            }
            g->setTextSize(2);
            g->setCursor(14, y + 4);
            g->print(labels[i]);

            // Show current values
            if (i == SI_STEP) {
                g->setTextSize(1);
                g->setCursor(130, y + 8);
                g->printf("=%d", s_throttleStep);
            }
        }

        g->setTextSize(1);
        g->setTextColor(RGB565_DARKGREY);
        g->setCursor(3, LCD_HEIGHT - 12);
        g->print("Clk=next Dbl=prev Hld=select");

        while (true) {
            ButtonEvent evt = Button::poll();
            if (evt == BTN_CLICK) { sel = (sel + 1) % SI_COUNT; break; }
            if (evt == BTN_DOUBLE_CLICK) { sel = (sel - 1 + SI_COUNT) % SI_COUNT; break; }
            if (evt == BTN_LONG_PRESS) {
                switch ((SetupItem)sel) {
                    case SI_ARM:
                        if (!s_armed) {
                            if (!DShot::init(MOTOR_PIN, s_dshotSpeed)) {
                                g->fillRect(0, 200, LCD_WIDTH, 30, RGB565_BLACK);
                                g->setTextSize(1);
                                g->setTextColor(RGB565_RED);
                                g->setCursor(5, 205);
                                g->print("DShot init FAILED!");
                                delay(1500);
                                break;
                            }
                            DShot::arm();
                            s_armed = true;
                            s_safetyLock = true;
                            s_throttle = 0;
                        } else {
                            s_throttle = 0;
                            // Send multiple zero frames for safe disarm
                            for (int i = 0; i < 50; i++) {
                                DShot::sendThrottle(0);
                                delayMicroseconds(2000);
                            }
                            DShot::stop();
                            s_armed = false;
                        }
                        break;
                    case SI_SPEED:
                        if (s_dshotSpeed == DSHOT150) s_dshotSpeed = DSHOT300;
                        else if (s_dshotSpeed == DSHOT300) s_dshotSpeed = DSHOT600;
                        else s_dshotSpeed = DSHOT150;
                        if (s_armed) {
                            DShot::stop();
                            DShot::init(MOTOR_PIN, s_dshotSpeed);
                        }
                        break;
                    case SI_BEEP:
                        if (s_armed) {
                            for (int i = 0; i < 50; i++) {
                                DShot::sendCommand(DSHOT_CMD_BEEP1);
                                delayMicroseconds(2000);
                            }
                        }
                        break;
                    case SI_STEP:
                        if (s_throttleStep >= 100) s_throttleStep = 10;
                        else if (s_throttleStep >= 50) s_throttleStep = 100;
                        else s_throttleStep = 50;
                        break;
                    case SI_BACK:
                        if (s_armed) {
                            s_throttle = 0;
                            DShot::sendThrottle(0);
                            delay(100);
                            DShot::stop();
                            s_armed = false;
                        }
                        return false; // exit to main menu
                    default: break;
                }
                return true; // stay in motor tester
            }
            delay(10);
        }
    }
}

void runMotorTester() {
    s_throttle = 0;
    s_armed = false;
    s_safetyLock = true;
    s_dshotSpeed = DSHOT300;

    drawControl();

    uint32_t lastSend = 0;

    while (true) {
        feed_wdt();
        ButtonEvent evt = Button::poll();

        if (!s_armed) {
            // Not armed — long press opens setup
            if (evt == BTN_LONG_PRESS) {
                if (!runSetup()) return; // exit to menu
                drawControl();
            }
        } else if (s_safetyLock) {
            // Armed but locked — click to unlock, DblClk to disarm
            if (evt == BTN_CLICK) {
                s_safetyLock = false;
                drawControl();
            } else if (evt == BTN_DOUBLE_CLICK) {
                // Disarm (quick exit from locked state)
                s_throttle = 0;
                for (int i = 0; i < 50; i++) {
                    DShot::sendThrottle(0);
                    delayMicroseconds(2000);
                }
                DShot::stop();
                s_armed = false;
                s_safetyLock = true;
                drawControl();
            } else if (evt == BTN_LONG_PRESS) {
                // Open setup (with disarm option inside)
                if (!runSetup()) return; // exit to menu
                drawControl();
            }
        } else {
            // Armed + unlocked (LIVE) — throttle control
            if (evt == BTN_CLICK) {
                s_throttle = min((int)s_throttle + s_throttleStep, 2047);
                updateThrottleDisplay();
            } else if (evt == BTN_DOUBLE_CLICK) {
                s_throttle = max((int)s_throttle - s_throttleStep, 0);
                updateThrottleDisplay();
            } else if (evt == BTN_LONG_PRESS) {
                // Emergency stop + setup (motor always stopped first)
                s_throttle = 0;
                for (int i = 0; i < 20; i++) {
                    DShot::sendThrottle(0);
                    delayMicroseconds(2000);
                }
                s_safetyLock = true;  // auto re-lock on leaving live mode
                updateThrottleDisplay();
                delay(200);
                if (!runSetup()) return; // exit to menu
                drawControl();
            }
        }

        // Send DShot frame continuously (~500Hz)
        // Map: UI 0=disarm, UI 1-2000 → DShot 48-2047
        if (s_armed && (micros() - lastSend > 2000)) {
            uint16_t dsVal = (s_throttle == 0) ? 0 : constrain(s_throttle + 47, 48, 2047);
            DShot::sendThrottle(dsVal);
            lastSend = micros();
        }

        delay(2);
    }
}
