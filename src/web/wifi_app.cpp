#include "wifi_app.h"
#include <Arduino.h>
#include <qrcode.h>
#include "ui/display.h"
#include "ui/button.h"
#include "ui/status_led.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "web_state.h"
#include "battery/dji_battery.h"
#include "motor/dshot.h"
#include "motor/motor_dispatch.h"
#include "pin_config.h"
#include "wdt.h"

static const char* AP_SSID = "FPV-MultiTool";
static const char* AP_PASS = "fpv12345";

// Draw QR code at x,y with given pixel scale
static void drawQR(int x, int y, const char *text, int scale = 4) {
    auto *g = Display::gfx();

    // QR version 4 (33x33) with ECC_LOW fits ~78 alphanumeric or ~62 chars
    // WiFi QR "WIFI:T:WPA;S:FPV-MultiTool;P:fpv12345;;" = 40 chars → fits v3 (29x29) or v4 (33x33)
    // URL "http://192.168.4.1" = 18 chars → fits v2 (25x25)
    QRCode qr;
    uint8_t qrData[qrcode_getBufferSize(4)];
    qrcode_initText(&qr, qrData, 4, ECC_LOW, text);

    int size = qr.size; // typically 33 for v4

    // White background for QR
    g->fillRect(x - 2, y - 2, size * scale + 4, size * scale + 4, RGB565_WHITE);

    for (int py = 0; py < size; py++) {
        for (int px = 0; px < size; px++) {
            if (qrcode_getModule(&qr, px, py)) {
                g->fillRect(x + px * scale, y + py * scale, scale, scale, RGB565_BLACK);
            }
        }
    }
}

static void drawStatus() {
    auto *g = Display::gfx();
    g->fillScreen(RGB565_BLACK);

    // Title
    g->setTextSize(2);
    g->setTextColor(RGB565_CYAN);
    g->setCursor(20, 4);
    g->println("WiFi / Web");
    g->drawFastHLine(0, 24, LCD_WIDTH, RGB565_DARKGREY);

    auto mode = WifiManager::currentMode();
    String ip = WifiManager::getIP();
    String ssid = WifiManager::getSSID();

    // Info block (compact, textSize 1)
    g->setTextSize(1);
    int y = 28;

    g->setTextColor(RGB565_YELLOW);
    g->setCursor(5, y);
    g->printf("%s  %s",
        mode == WifiManager::MODE_AP ? "AP" :
        mode == WifiManager::MODE_STA ? "STA" : "OFF",
        ssid.c_str());
    y += 12;

    g->setTextColor(RGB565_GREEN);
    g->setCursor(5, y);
    g->printf("%s", ip.c_str());
    y += 12;

    if (mode == WifiManager::MODE_AP) {
        g->setTextColor(RGB565_DARKGREY);
        g->setCursor(5, y);
        g->printf("pass:%s  cli:%d", AP_PASS, WifiManager::clientCount());
        y += 12;
    } else if (mode == WifiManager::MODE_STA) {
        g->setTextColor(RGB565_DARKGREY);
        g->setCursor(5, y);
        g->printf("RSSI: %d dBm", WifiManager::getRSSI());
        y += 12;
    }

    y += 4;

    // QR code
    // For AP: WiFi QR auto-joins network when scanned
    // For STA: URL QR opens browser directly
    int qrScale = 4;
    int qrSize = 33 * qrScale; // v4 = 33 modules × 4 = 132px
    int qrX = (LCD_WIDTH - qrSize) / 2;
    int qrY = y + 2;

    if (mode != WifiManager::MODE_OFF && ip.length() > 0) {
        char qrText[128];
        if (mode == WifiManager::MODE_AP) {
            // WiFi QR format: WIFI:T:WPA;S:<ssid>;P:<pass>;;
            snprintf(qrText, sizeof(qrText), "WIFI:T:WPA;S:%s;P:%s;;", AP_SSID, AP_PASS);
        } else {
            // STA: URL QR opens browser
            snprintf(qrText, sizeof(qrText), "http://%s", ip.c_str());
        }
        drawQR(qrX, qrY, qrText, qrScale);

        // Caption below QR
        int capY = qrY + qrSize + 4;
        g->setTextSize(1);
        g->setTextColor(RGB565_CYAN);
        g->setCursor(0, capY);
        if (mode == WifiManager::MODE_AP) {
            // Center text
            const char* cap = "Scan to connect WiFi";
            int w = strlen(cap) * 6;
            g->setCursor((LCD_WIDTH - w) / 2, capY);
            g->print(cap);
            // Second line with URL
            char urlLine[32];
            snprintf(urlLine, sizeof(urlLine), "then open: %s", ip.c_str());
            int w2 = strlen(urlLine) * 6;
            g->setCursor((LCD_WIDTH - w2) / 2, capY + 12);
            g->setTextColor(RGB565_YELLOW);
            g->print(urlLine);
        } else {
            const char* cap = "Scan to open in browser";
            int w = strlen(cap) * 6;
            g->setCursor((LCD_WIDTH - w) / 2, capY);
            g->print(cap);
        }
    }

    // Footer
    g->setTextSize(1);
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(3, LCD_HEIGHT - 24);
    g->printf("Heap:%dKB  Web:%s",
        ESP.getFreeHeap() / 1024,
        WebServer::isRunning() ? "ON" : "OFF");
    g->setCursor(3, LCD_HEIGHT - 12);
    g->print("Clk=refresh DblClk=back");
}

void runWifiApp() {
    drawStatus();

    uint32_t lastRedraw = millis();

    while (true) {
        feed_wdt();
        StatusLed::loop();
        ButtonEvent evt = Button::poll();

        if (evt == BTN_DOUBLE_CLICK) {
            return;
        }
        if (evt == BTN_CLICK) {
            drawStatus();
        }
        if (evt == BTN_LONG_PRESS) {
            auto *g = Display::gfx();
            g->fillScreen(RGB565_BLACK);
            g->setTextSize(2);
            g->setTextColor(RGB565_YELLOW);
            g->setCursor(10, 40);
            g->println("Restarting...");

            WebServer::stop();
            WifiManager::stop();
            delay(500);

            String ssid, pass;
            bool ok = false;
            if (WifiManager::loadCredentials(ssid, pass) && ssid.length() > 0) {
                ok = WifiManager::startSTA(ssid.c_str(), pass.c_str(), 10000);
            }
            if (!ok) WifiManager::startAP(AP_SSID, AP_PASS);
            WebServer::start();
            drawStatus();
        }

        // Keep web server + motor/servo commands alive via shared pump.
        WebServer::loop();
        MotorDispatch::pump();

        // Refresh status every 3s (keep QR rendered)
        if (millis() - lastRedraw > 3000) {
            drawStatus();
            lastRedraw = millis();
        }

        delay(2);
    }
}
