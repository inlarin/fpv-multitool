#include "motor_tester.h"
#include <Arduino.h>
#include "pin_config.h"
#include "ui/display.h"
#include "ui/button.h"
#include "dshot.h"
#include "wdt.h"
#include "web/web_state.h"

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
static uint16_t s_throttle = 0;     // 0-2000 (UI), maps to DShot 48-2047
static bool s_armed = false;
static bool s_safetyLock = true;    // Prevent accidental throttle
static int s_throttleStep = 20;     // default 1% of 2000
static uint16_t s_maxThrottle = 2000; // safety cap

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
    // Throttle % in top-right (relative to full UI range 2000)
    int pct = s_throttle * 100 / 2000;
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
    int pct = s_throttle * 100 / 2000;
    g->setTextSize(2);
    g->setTextColor(RGB565_CYAN);
    g->setCursor(60, 115);
    g->printf("%3d%%", pct);

    // Throttle bar
    int barY = 145;
    int barH = 16;
    int barW = LCD_WIDTH - 20;
    g->drawRect(9, barY - 1, barW + 2, barH + 2, RGB565_DARKGREY);

    int fillW = s_throttle * barW / 2000;
    uint16_t barColor = RGB565_GREEN;
    if (pct > 50) barColor = RGB565_YELLOW;
    if (pct > 80) barColor = RGB565_RED;
    if (fillW > 0) g->fillRect(10, barY, fillW, barH, barColor);

    // Cap marker (vertical line showing s_maxThrottle position)
    if (s_maxThrottle < 2000) {
        int capX = 10 + s_maxThrottle * barW / 2000;
        g->drawFastVLine(capX, barY - 2, barH + 4, RGB565_ORANGE);
    }

    // Scale
    g->setTextSize(1);
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(5, barY + barH + 4);
    g->print("0");
    g->setCursor(70, barY + barH + 4);
    g->print("1000");
    g->setCursor(140, barY + barH + 4);
    g->print("2000");

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
    int pct = s_throttle * 100 / 2000;
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
    int fillW = s_throttle * barW / 2000;
    uint16_t barColor = RGB565_GREEN;
    if (pct > 50) barColor = RGB565_YELLOW;
    if (pct > 80) barColor = RGB565_RED;
    if (fillW > 0) g->fillRect(10, barY, fillW, barH, barColor);

    // Cap marker
    if (s_maxThrottle < 2000) {
        int capX = 10 + s_maxThrottle * barW / 2000;
        g->drawFastVLine(capX, barY - 2, barH + 4, RGB565_ORANGE);
    }
}

// Setup menu: expanded with direction, 3D mode, power limit
enum SetupItem {
    SI_ARM, SI_SPEED, SI_BEEP,
    SI_DIR_CW, SI_DIR_CCW,
    SI_3D_ON, SI_3D_OFF,
    SI_STEP, SI_MAX_THROTTLE,
    SI_BACK, SI_COUNT
};

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
        labels[SI_DIR_CW]  = "Dir: CW (normal)";
        labels[SI_DIR_CCW] = "Dir: CCW (reverse)";
        labels[SI_3D_ON]   = "3D mode ON";
        labels[SI_3D_OFF]  = "3D mode OFF";
        labels[SI_STEP]    = "Step size";
        labels[SI_MAX_THROTTLE] = "Max throttle";
        labels[SI_BACK]    = "<< Menu";

        // Compact layout for 10 items: smaller rows
        int itemH = (LCD_HEIGHT - 40 - 20) / SI_COUNT;
        if (itemH < 16) itemH = 16;

        for (int i = 0; i < SI_COUNT; i++) {
            int y = 30 + i * itemH;
            if (i == sel) {
                uint16_t bg = (i == SI_BACK) ? RGB565_MAROON : RGB565_NAVY;
                g->fillRoundRect(4, y - 1, LCD_WIDTH - 8, itemH - 2, 3, bg);
                g->setTextColor(RGB565_WHITE);
            } else {
                g->setTextColor(RGB565_DARKGREY);
            }
            g->setTextSize(1);
            g->setCursor(10, y + (itemH - 8) / 2);
            g->print(labels[i]);

            // Show current values
            if (i == SI_STEP) {
                g->setCursor(130, y + (itemH - 8) / 2);
                g->printf("=%d", s_throttleStep);
            } else if (i == SI_MAX_THROTTLE) {
                g->setCursor(130, y + (itemH - 8) / 2);
                g->printf("=%d", s_maxThrottle);
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
                    case SI_DIR_CW:
                    case SI_DIR_CCW: {
                        // Spin direction commands require ESC armed (zero throttle pulses).
                        // Must be sent >=6 times to be accepted; we send 10 to be safe.
                        if (s_armed) {
                            uint8_t cmd = (sel == SI_DIR_CW) ? DSHOT_CMD_SPIN_DIR_1
                                                             : DSHOT_CMD_SPIN_DIR_2;
                            for (int i = 0; i < 10; i++) {
                                DShot::sendCommand(cmd);
                                delay(1);
                            }
                        }
                        break;
                    }
                    case SI_3D_ON:
                    case SI_3D_OFF: {
                        if (s_armed) {
                            uint8_t cmd = (sel == SI_3D_ON) ? DSHOT_CMD_3D_MODE_ON
                                                            : DSHOT_CMD_3D_MODE_OFF;
                            for (int i = 0; i < 10; i++) {
                                DShot::sendCommand(cmd);
                                delay(1);
                            }
                        }
                        break;
                    }
                    case SI_STEP:
                        // Cycle: 10 → 20 → 50 → 100 → 200 → 10
                        if (s_throttleStep < 20) s_throttleStep = 20;
                        else if (s_throttleStep < 50) s_throttleStep = 50;
                        else if (s_throttleStep < 100) s_throttleStep = 100;
                        else if (s_throttleStep < 200) s_throttleStep = 200;
                        else s_throttleStep = 10;
                        break;
                    case SI_MAX_THROTTLE:
                        // Cycle: 500 → 1000 → 1500 → 2000 → 500
                        if (s_maxThrottle < 1000) s_maxThrottle = 1000;
                        else if (s_maxThrottle < 1500) s_maxThrottle = 1500;
                        else if (s_maxThrottle < 2000) s_maxThrottle = 2000;
                        else s_maxThrottle = 500;
                        // Clamp current throttle if now above cap
                        if (s_throttle > s_maxThrottle) s_throttle = s_maxThrottle;
                        // Propagate to web shared state
                        { WebState::Lock lock; WebState::motor.maxThrottle = s_maxThrottle; }
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
    // Inherit max throttle from shared state (web may have set it)
    {
        WebState::Lock lock;
        s_maxThrottle = constrain(WebState::motor.maxThrottle, 100, 2000);
    }

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
                s_throttle = min((int)s_throttle + s_throttleStep, (int)s_maxThrottle);
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
