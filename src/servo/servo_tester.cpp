#include "servo_tester.h"
#include <Arduino.h>
#include "pin_config.h"
#include "ui/display.h"
#include "ui/button.h"

// === Servo config ===
static const int SERVO_PIN = SIGNAL_OUT;  // GPIO 1
static const int PWM_CHANNEL = 0;
static const int PWM_RESOLUTION = 16;    // 16-bit for fine control

enum ServoMode { MODE_MANUAL, MODE_CENTER, MODE_SWEEP, MODE_COUNT };
static const char* MODE_NAMES[] = {"Manual", "Center", "Sweep"};

static ServoMode s_mode = MODE_MANUAL;
static int s_pulseUs = 1500;     // current pulse width in microseconds
static int s_freq = 50;          // PWM frequency (50 or 330 Hz)
static int s_sweepDir = 1;       // sweep direction
static int s_sweepSpeed = 5;     // us per step
static int s_stepSize = 10;      // manual step size in us

static const int PULSE_MIN = 500;
static const int PULSE_MAX = 2500;
static const int PULSE_CENTER = 1500;

// === PWM helpers (Arduino Core 3.x API) ===
static void pwmSetup() {
    ledcAttach(SERVO_PIN, s_freq, PWM_RESOLUTION);
}

static void pwmUpdate() {
    uint32_t period_us = 1000000 / s_freq;
    uint32_t max_duty = (1 << PWM_RESOLUTION) - 1;
    uint32_t duty = (uint32_t)s_pulseUs * max_duty / period_us;
    ledcWrite(SERVO_PIN, duty);
}

static void pwmStop() {
    ledcWrite(SERVO_PIN, 0);
    ledcDetach(SERVO_PIN);
}

// === Drawing ===
static void drawUI() {
    auto *g = Display::gfx();
    g->fillScreen(RGB565_BLACK);

    // Title
    g->setTextSize(2);
    g->setTextColor(RGB565_MAGENTA);
    g->setCursor(5, 4);
    g->print("Servo Test");
    g->drawFastHLine(0, 24, LCD_WIDTH, RGB565_DARKGREY);

    // Mode
    g->setTextSize(1);
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(5, 30);
    g->printf("Mode: %s  Freq: %dHz", MODE_NAMES[s_mode], s_freq);

    // Big pulse display
    g->setTextSize(3);
    g->setTextColor(RGB565_WHITE);
    g->setCursor(15, 55);
    g->printf("%4dus", s_pulseUs);

    // Percentage
    int pct = (s_pulseUs - PULSE_MIN) * 100 / (PULSE_MAX - PULSE_MIN);
    g->setTextSize(2);
    g->setTextColor(RGB565_CYAN);
    g->setCursor(55, 90);
    g->printf("%3d%%", pct);

    // Visual bar
    int barY = 120;
    int barH = 20;
    int barW = LCD_WIDTH - 20;
    g->drawRect(9, barY - 1, barW + 2, barH + 2, RGB565_DARKGREY);

    int fillW = (s_pulseUs - PULSE_MIN) * barW / (PULSE_MAX - PULSE_MIN);
    uint16_t barColor = RGB565_GREEN;
    if (s_pulseUs < 1000 || s_pulseUs > 2000) barColor = RGB565_ORANGE;
    if (s_pulseUs <= PULSE_MIN || s_pulseUs >= PULSE_MAX) barColor = RGB565_RED;
    g->fillRect(10, barY, fillW, barH, barColor);

    // Scale labels
    g->setTextSize(1);
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(5, barY + barH + 5);
    g->print("500");
    g->setCursor(65, barY + barH + 5);
    g->print("1500");
    g->setCursor(135, barY + barH + 5);
    g->print("2500");

    // Center mark on bar
    int centerX = 10 + (PULSE_CENTER - PULSE_MIN) * barW / (PULSE_MAX - PULSE_MIN);
    g->drawFastVLine(centerX, barY - 3, barH + 6, RGB565_YELLOW);

    // Pin info
    g->setTextSize(1);
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(5, 175);
    g->printf("Output: GPIO%d", SERVO_PIN);
    g->setCursor(5, 188);
    g->printf("Step: %dus", s_stepSize);

    // Sweep info
    if (s_mode == MODE_SWEEP) {
        g->setTextColor(RGB565_GREEN);
        g->setCursor(5, 205);
        g->printf("Sweep: %d-%d  spd=%d",
            PULSE_MIN, PULSE_MAX, s_sweepSpeed);
    }

    // Footer
    g->setTextSize(1);
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(3, LCD_HEIGHT - 24);
    g->print("Clk=+  Dbl=-  Hld=mode");
    g->setCursor(3, LCD_HEIGHT - 12);
    g->print("In mode select: Hld=back");
}

// Quick update of pulse value without full redraw
static void updatePulseDisplay() {
    auto *g = Display::gfx();

    // Pulse number
    g->fillRect(15, 55, 145, 28, RGB565_BLACK);
    g->setTextSize(3);
    g->setTextColor(RGB565_WHITE);
    g->setCursor(15, 55);
    g->printf("%4dus", s_pulseUs);

    // Percentage
    int pct = (s_pulseUs - PULSE_MIN) * 100 / (PULSE_MAX - PULSE_MIN);
    g->fillRect(55, 90, 80, 20, RGB565_BLACK);
    g->setTextSize(2);
    g->setTextColor(RGB565_CYAN);
    g->setCursor(55, 90);
    g->printf("%3d%%", pct);

    // Bar
    int barY = 120;
    int barH = 20;
    int barW = LCD_WIDTH - 20;
    g->fillRect(10, barY, barW, barH, RGB565_BLACK);

    int fillW = (s_pulseUs - PULSE_MIN) * barW / (PULSE_MAX - PULSE_MIN);
    uint16_t barColor = RGB565_GREEN;
    if (s_pulseUs < 1000 || s_pulseUs > 2000) barColor = RGB565_ORANGE;
    if (s_pulseUs <= PULSE_MIN || s_pulseUs >= PULSE_MAX) barColor = RGB565_RED;
    g->fillRect(10, barY, fillW, barH, barColor);

    // Center mark
    int centerX = 10 + (PULSE_CENTER - PULSE_MIN) * barW / (PULSE_MAX - PULSE_MIN);
    g->drawFastVLine(centerX, barY - 3, barH + 6, RGB565_YELLOW);
}

// === Mode selection submenu ===
// Returns true = mode selected, false = exit to menu
static bool selectMode() {
    int sel = (int)s_mode;
    int itemCount = MODE_COUNT + 2; // modes + freq + back
    auto *g = Display::gfx();

    while (true) {
        g->fillScreen(RGB565_BLACK);
        g->setTextSize(2);
        g->setTextColor(RGB565_MAGENTA);
        g->setCursor(10, 10);
        g->println("Servo Setup");
        g->drawFastHLine(0, 32, LCD_WIDTH, RGB565_DARKGREY);

        for (int i = 0; i < MODE_COUNT; i++) {
            int y = 42 + i * 34;
            if (i == sel) {
                g->fillRoundRect(6, y, LCD_WIDTH - 12, 28, 4, RGB565_NAVY);
                g->setTextColor(RGB565_WHITE);
            } else {
                g->setTextColor(RGB565_DARKGREY);
            }
            g->setTextSize(2);
            g->setCursor(14, y + 5);
            g->print(MODE_NAMES[i]);
        }

        // Freq toggle
        int fy = 42 + MODE_COUNT * 34;
        if (sel == MODE_COUNT) {
            g->fillRoundRect(6, fy, LCD_WIDTH - 12, 28, 4, RGB565_NAVY);
            g->setTextColor(RGB565_WHITE);
        } else {
            g->setTextColor(RGB565_DARKGREY);
        }
        g->setTextSize(2);
        g->setCursor(14, fy + 5);
        g->printf("Freq:%dHz", s_freq);

        // Back option
        int by = 42 + (MODE_COUNT + 1) * 34;
        if (sel == MODE_COUNT + 1) {
            g->fillRoundRect(6, by, LCD_WIDTH - 12, 28, 4, RGB565_MAROON);
            g->setTextColor(RGB565_WHITE);
        } else {
            g->setTextColor(RGB565_DARKGREY);
        }
        g->setTextSize(2);
        g->setCursor(14, by + 5);
        g->print("<< Menu");

        g->setTextSize(1);
        g->setTextColor(RGB565_DARKGREY);
        g->setCursor(3, LCD_HEIGHT - 12);
        g->print("Clk=next Dbl=prev Hld=select");

        while (true) {
            ButtonEvent evt = Button::poll();
            if (evt == BTN_CLICK) {
                sel = (sel + 1) % itemCount;
                break;
            } else if (evt == BTN_DOUBLE_CLICK) {
                sel = (sel - 1 + itemCount) % itemCount;
                break;
            } else if (evt == BTN_LONG_PRESS) {
                if (sel < MODE_COUNT) {
                    s_mode = (ServoMode)sel;
                    if (s_mode == MODE_CENTER) s_pulseUs = PULSE_CENTER;
                    return true;
                } else if (sel == MODE_COUNT) {
                    // Stop PWM before changing frequency
                    pwmStop();
                    s_freq = (s_freq == 50) ? 330 : 50;
                    pwmSetup();
                    pwmUpdate();
                    break;
                } else {
                    return false; // back to main menu
                }
            }
            delay(10);
        }
    }
}

// === Main entry ===
void runServoTester() {
    s_pulseUs = PULSE_CENTER;
    s_mode = MODE_MANUAL;
    s_freq = 50;
    s_sweepDir = 1;

    pwmSetup();
    pwmUpdate();
    drawUI();

    uint32_t lastSweep = 0;

    while (true) {
        ButtonEvent evt = Button::poll();

        if (evt == BTN_LONG_PRESS) {
            pwmStop();
            if (!selectMode()) {
                // User chose "<< Menu" — exit to main menu
                pinMode(SERVO_PIN, INPUT);
                return;
            }
            pwmSetup();
            pwmUpdate();
            drawUI();
        }

        switch (s_mode) {
            case MODE_MANUAL:
                if (evt == BTN_CLICK) {
                    s_pulseUs = min(s_pulseUs + s_stepSize, PULSE_MAX);
                    pwmUpdate();
                    updatePulseDisplay();
                } else if (evt == BTN_DOUBLE_CLICK) {
                    s_pulseUs = max(s_pulseUs - s_stepSize, PULSE_MIN);
                    pwmUpdate();
                    updatePulseDisplay();
                }
                break;

            case MODE_CENTER:
                s_pulseUs = PULSE_CENTER;
                pwmUpdate();
                break;

            case MODE_SWEEP:
                if (millis() - lastSweep > 20) {
                    s_pulseUs += s_sweepDir * s_sweepSpeed;
                    if (s_pulseUs >= PULSE_MAX) {
                        s_pulseUs = PULSE_MAX;
                        s_sweepDir = -1;
                    } else if (s_pulseUs <= PULSE_MIN) {
                        s_pulseUs = PULSE_MIN;
                        s_sweepDir = 1;
                    }
                    pwmUpdate();
                    updatePulseDisplay();
                    lastSweep = millis();
                }
                // Click/DblClick adjust sweep speed
                if (evt == BTN_CLICK) {
                    s_sweepSpeed = min(s_sweepSpeed + 2, 30);
                    drawUI();
                } else if (evt == BTN_DOUBLE_CLICK) {
                    s_sweepSpeed = max(s_sweepSpeed - 2, 1);
                    drawUI();
                }
                break;

            default: break;
        }

        delay(5);
    }
}
