#include "usb2ttl.h"
#include <Arduino.h>
#include "pin_config.h"
#include "ui/display.h"
#include "ui/button.h"
#include "wdt.h"

// USB CDC access for DTR/RTS signals
#if ARDUINO_USB_CDC_ON_BOOT
#include "USB.h"
#include "USBCDC.h"
extern USBCDC USBSerial;
#endif

static HardwareSerial &uart = Serial1;

// === State ===
enum TTLPage { PAGE_STATUS, PAGE_PINOUT, PAGE_MONITOR, PAGE_COUNT };

static TTLPage s_page = PAGE_STATUS;
static bool s_bridgeActive = false;
static uint32_t s_currentBaud = 0;
static uint32_t s_txCount = 0;
static uint32_t s_rxCount = 0;
static bool s_deviceDetected = false;

// Serial monitor ring buffer (last N lines)
static const int MON_LINES = 16;
static const int MON_LINE_LEN = 28; // fits 172px at textSize=1
static char s_monBuf[MON_LINES][MON_LINE_LEN + 1];
static int s_monHead = 0;
static int s_monCount = 0;
static char s_monLineBuf[MON_LINE_LEN + 1];
static int s_monLinePos = 0;

// DTR/RTS state tracking
static bool s_lastDTR = false;
static bool s_lastRTS = false;

// === Drawing helpers ===
static void drawHeader(const char* subtitle) {
    auto *g = Display::gfx();
    g->setTextSize(2);
    g->setTextColor(RGB565_GREEN);
    g->setCursor(5, 4);
    g->print("USB2TTL");
    g->setTextSize(1);
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(95, 10);
    g->print(subtitle);
    g->drawFastHLine(0, 24, LCD_WIDTH, RGB565_DARKGREY);
}

static void drawFooter() {
    auto *g = Display::gfx();
    int y = LCD_HEIGHT - 12;
    g->fillRect(0, y - 2, LCD_WIDTH, 14, RGB565_BLACK);
    g->setTextSize(1);
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(3, y);
    if (s_bridgeActive) {
        g->print("Clk=page Hld=stop");
    } else {
        g->print("Clk=page Hld=start Dbl=back");
    }
}

// --- PAGE: Status ---
static void drawStatus() {
    auto *g = Display::gfx();
    g->fillScreen(RGB565_BLACK);
    drawHeader("status");

    int y = 30;
    g->setTextSize(1);

    // Bridge status
    g->setCursor(5, y);
    if (s_bridgeActive) {
        g->setTextColor(RGB565_GREEN);
        g->print(">> BRIDGE ACTIVE <<");
    } else {
        g->setTextColor(RGB565_YELLOW);
        g->print("Bridge: OFF");
    }
    y += 16;

    // Baud
    g->setTextColor(RGB565_WHITE);
    g->setCursor(5, y);
    if (s_currentBaud > 0) {
        g->printf("Baud: %lu (auto)", s_currentBaud);
    } else {
        g->print("Baud: auto-detect");
    }
    y += 16;

    // Counters
    g->setTextColor(RGB565_CYAN);
    g->setCursor(5, y);
    g->printf("TX: %lu  RX: %lu", s_txCount, s_rxCount);
    y += 16;

    // DTR/RTS
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(5, y);
    g->printf("DTR:%s RTS:%s",
        s_lastDTR ? "ON " : "off",
        s_lastRTS ? "ON " : "off");
    y += 20;

    // Connection
    g->setCursor(5, y);
    if (s_bridgeActive && s_rxCount > 0) {
        g->setTextColor(RGB565_GREEN);
        g->print("Device: responding");
    } else if (s_bridgeActive) {
        g->setTextColor(RGB565_RED);
        g->print("Device: no response");
    } else {
        g->setTextColor(RGB565_DARKGREY);
        g->print("Device: -");
    }
    y += 20;

    // Voltage warning
    g->setTextColor(RGB565_RED);
    g->setCursor(5, y);
    g->print("! Use 3.3V for receiver !");

    drawFooter();
}

// --- PAGE: Pinout ---
static void drawPinout() {
    auto *g = Display::gfx();
    g->fillScreen(RGB565_BLACK);
    drawHeader("wiring");

    int y = 30;
    g->setTextSize(1);
    g->setTextColor(RGB565_WHITE);

    // Wiring table
    g->setCursor(5, y);
    g->setTextColor(RGB565_CYAN);
    g->print("ESP32       Receiver");
    y += 14;

    g->drawFastHLine(5, y, 162, RGB565_DARKGREY);
    y += 4;

    struct { const char* esp; const char* rx; uint16_t color; } wires[] = {
        {"GPIO43 TX", "RX",     RGB565_YELLOW},
        {"GPIO44 RX", "TX",     RGB565_GREEN},
        {"GPIO3 BOOT","GPIO0",  RGB565_ORANGE},
        {"5V",        "VCC",    RGB565_RED},
        {"GND",       "GND",    RGB565_DARKGREY},
    };

    for (int i = 0; i < 5; i++) {
        g->setCursor(5, y);
        g->setTextColor(wires[i].color);
        g->printf("%-11s", wires[i].esp);
        g->setTextColor(RGB565_DARKGREY);
        g->print(" -> ");
        g->setTextColor(wires[i].color);
        g->print(wires[i].rx);
        y += 13;
    }

    y += 8;
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(5, y);
    g->print("Common ELRS receivers:");
    y += 12;
    g->setTextColor(RGB565_WHITE);
    g->setCursor(5, y); g->print("EP1/EP2: VCC TX RX GND");
    y += 11;
    g->setCursor(5, y); g->print("RP1/RP2: VCC GND TX RX");
    y += 11;
    g->setCursor(5, y); g->print("BetaFPV: GND VCC TX RX");

    y += 16;
    g->setTextColor(RGB565_RED);
    g->setCursor(5, y);
    g->print("! TX/RX are CROSSED !");
    y += 11;
    g->setCursor(5, y);
    g->print("  ESP TX -> RX receiver");

    drawFooter();
}

// --- PAGE: Serial Monitor ---
static void monAddChar(char c) {
    if (c == '\n' || s_monLinePos >= MON_LINE_LEN) {
        s_monLineBuf[s_monLinePos] = '\0';
        strncpy(s_monBuf[s_monHead], s_monLineBuf, MON_LINE_LEN + 1);
        s_monHead = (s_monHead + 1) % MON_LINES;
        if (s_monCount < MON_LINES) s_monCount++;
        s_monLinePos = 0;
    } else if (c >= 0x20) { // printable
        s_monLineBuf[s_monLinePos++] = c;
    }
}

static void drawMonitor() {
    auto *g = Display::gfx();
    g->fillScreen(RGB565_BLACK);
    drawHeader("monitor");

    g->setTextSize(1);
    g->setTextColor(RGB565_GREEN);

    int startLine = (s_monHead - s_monCount + MON_LINES) % MON_LINES;
    int y = 28;
    for (int i = 0; i < s_monCount && y < LCD_HEIGHT - 16; i++) {
        int idx = (startLine + i) % MON_LINES;
        g->setCursor(2, y);
        g->print(s_monBuf[idx]);
        y += 10;
    }

    if (s_monCount == 0) {
        g->setTextColor(RGB565_DARKGREY);
        g->setCursor(5, 80);
        g->print("No data yet...");
        g->setCursor(5, 95);
        g->print("UART output appears here");
    }

    drawFooter();
}

// === DTR/RTS auto-reset (esptool boot sequence) ===
static void handleDTRRTS() {
#if ARDUINO_USB_CDC_ON_BOOT
    // On ESP32-S3 with Arduino Core 3.x, Serial is HWCDC
    // DTR/RTS not directly accessible via HWCDC API in all versions
    // Fallback: we keep BOOT pin low during bridge (manual mode)
#endif
    // TODO: when HWCDC exposes DTR/RTS callbacks, wire them to ELRS_BOOT/RST
    // For now, BOOT pin is held low during bridge = flash mode
}

// === Redraw current page ===
static void redrawPage() {
    switch (s_page) {
        case PAGE_STATUS:  drawStatus(); break;
        case PAGE_PINOUT:  drawPinout(); break;
        case PAGE_MONITOR: drawMonitor(); break;
        default: break;
    }
}

// === Main entry ===
void runUSB2TTL() {
    s_bridgeActive = false;
    s_txCount = 0;
    s_rxCount = 0;
    s_currentBaud = 0;
    s_page = PAGE_STATUS;
    s_monCount = 0;
    s_monHead = 0;
    s_monLinePos = 0;

    pinMode(ELRS_BOOT, OUTPUT);
    digitalWrite(ELRS_BOOT, HIGH);

    redrawPage();

    uint32_t lastUpdate = 0;

    while (true) {
        feed_wdt();
        ButtonEvent evt = Button::poll();

        // --- Navigation ---
        if (evt == BTN_CLICK) {
            s_page = (TTLPage)((s_page + 1) % PAGE_COUNT);
            redrawPage();
        }

        if (!s_bridgeActive) {
            if (evt == BTN_DOUBLE_CLICK) {
                uart.end();
                pinMode(ELRS_BOOT, INPUT);
                return;
            }
            if (evt == BTN_LONG_PRESS) {
                // Start bridge
                s_bridgeActive = true;
                s_txCount = 0;
                s_rxCount = 0;
                s_currentBaud = 420000;

                uart.begin(s_currentBaud, SERIAL_8N1, ELRS_RX, ELRS_TX);
                uart.setRxBufferSize(4096);

                // Enter flash mode: pull BOOT low
                digitalWrite(ELRS_BOOT, LOW);
                delay(50);

                redrawPage();
            }
            delay(10);
        } else {
            // === Auto baud ===
            uint32_t hostBaud = Serial.baudRate();
            if (hostBaud > 0 && hostBaud != s_currentBaud) {
                s_currentBaud = hostBaud;
                uart.updateBaudRate(s_currentBaud);
            }

            // === Bridge: forward data ===
            uint8_t buf[512];

            int avail = Serial.available();
            if (avail > 0) {
                int n = Serial.readBytes(buf, min(avail, (int)sizeof(buf)));
                uart.write(buf, n);
                s_txCount += n;
            }

            avail = uart.available();
            if (avail > 0) {
                int n = uart.readBytes(buf, min(avail, (int)sizeof(buf)));
                Serial.write(buf, n);
                s_rxCount += n;

                // Feed serial monitor
                for (int i = 0; i < n; i++) monAddChar((char)buf[i]);
            }

            handleDTRRTS();

            // Update display periodically
            if (millis() - lastUpdate > 500) {
                if (s_page == PAGE_STATUS) drawStatus();
                else if (s_page == PAGE_MONITOR) drawMonitor();
                lastUpdate = millis();
            }

            // Stop bridge
            if (evt == BTN_LONG_PRESS) {
                s_bridgeActive = false;
                uart.end();
                digitalWrite(ELRS_BOOT, HIGH);
                s_currentBaud = 0;
                redrawPage();
            }
        }
    }
}
