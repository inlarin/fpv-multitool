#include "smbus_bridge_ui.h"
#include "battery/smbus_bridge.h"            // SMBusBridge::* + BridgeStats::*
#include "board/wsh_s3_lcd_147b/display.h"
#include "board/wsh_s3_lcd_147b/button.h"
#include <esp_task_wdt.h>

// BridgeStats storage moved to smbus_bridge.cpp -- see comment there.

static void drawFrame() {
    auto *g = Display::gfx();
    g->fillScreen(0x0000);
    g->setTextSize(2);
    g->setCursor(10, 8);
    g->setTextColor(0x07FF); // cyan
    g->println("USB2SMBus Bridge");
    g->drawFastHLine(0, 32, 172, 0x4208);

    g->setTextSize(1);
    g->setCursor(10, 50);
    g->setTextColor(0xFFFF);
    g->println("PC <-> ESP32 <-> battery");
    g->setCursor(10, 62);
    g->setTextColor(0x7BEF);
    g->println("@115200 bin proto");

    g->setCursor(10, 78);
    g->setTextColor(0x07E0); // green
    g->println("Listening for cmds");

    g->setCursor(10, 296);
    g->setTextColor(0x7BEF);
    g->println("Long BOOT = exit");
}

static void drawStats() {
    auto *g = Display::gfx();
    int y = 110;
    g->setTextSize(1);
    g->fillRect(0, y, 172, 170, 0x0000);
    g->setCursor(10, y);
    g->setTextColor(0xFFE0); // yellow
    g->printf("Writes : %u\n", (unsigned)BridgeStats::cmdWrite); y += 12;
    g->setCursor(10, y);
    g->printf("ReadW  : %u\n", (unsigned)BridgeStats::cmdRead); y += 12;
    g->setCursor(10, y);
    g->printf("AddrRd : %u\n", (unsigned)BridgeStats::cmdAddrRead); y += 12;
    g->setCursor(10, y);
    g->printf("Pings  : %u\n", (unsigned)BridgeStats::cmdPing); y += 12;
    g->setCursor(10, y);
    g->setTextColor(BridgeStats::errCrc ? 0xF800 : 0x7BEF);
    g->printf("CRCerr : %u\n", (unsigned)BridgeStats::errCrc); y += 12;
    g->setCursor(10, y);
    g->setTextColor(BridgeStats::errI2C ? 0xF800 : 0x7BEF);
    g->printf("I2Cerr : %u\n", (unsigned)BridgeStats::errI2C); y += 16;

    g->setTextColor(0x07FF);
    g->setCursor(10, y);
    g->printf("last slv=0x%02X\n", (unsigned)BridgeStats::lastSlave); y += 12;
    g->setCursor(10, y);
    g->printf("last reg=0x%02X\n", (unsigned)BridgeStats::lastReg); y += 12;
    g->setCursor(10, y);
    g->printf("last st =0x%02X\n", (unsigned)BridgeStats::lastStatus);
}

void runUSB2SMBus() {
    BridgeStats::reset();
    drawFrame();
    drawStats();

    uint32_t lastDraw = 0;
    uint32_t lastWr = 0, lastRd = 0, lastAR = 0, lastPg = 0;
    while (true) {
        esp_task_wdt_reset();
        SMBusBridge::loop();
        ButtonEvent evt = Button::poll();
        if (evt == BTN_LONG_PRESS) break;

        // Redraw stats every 200ms if something changed
        if (millis() - lastDraw > 200) {
            if (BridgeStats::cmdWrite != lastWr ||
                BridgeStats::cmdRead != lastRd ||
                BridgeStats::cmdAddrRead != lastAR ||
                BridgeStats::cmdPing != lastPg) {
                drawStats();
                lastWr = BridgeStats::cmdWrite;
                lastRd = BridgeStats::cmdRead;
                lastAR = BridgeStats::cmdAddrRead;
                lastPg = BridgeStats::cmdPing;
            }
            lastDraw = millis();
        }
        delay(1);
    }
}
