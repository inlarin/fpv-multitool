#include "battery_ui.h"
#include <Arduino.h>
#include "pin_config.h"
#include "ui/display.h"
#include "ui/button.h"
#include "ui/status_led.h"
#include "dji_battery.h"
#include "wdt.h"

enum BattPage {
    BP_INFO,       // Main: SOC, V, I, T, cycles
    BP_CELLS,      // Cell voltages with balance
    BP_STATUS,     // PF/Safety/Operation status decoded
    BP_LIFETIME,   // Model, chip, FW, serial
    BP_SERVICE,    // Unseal / Clear PF / Seal
    BP_SCAN,       // I2C scanner
    BP_COUNT
};

static BattPage s_page = BP_INFO;
static BatteryInfo s_info = {};
static bool s_connected = false;

static void drawHeader() {
    auto *g = Display::gfx();
    g->setTextSize(2);
    g->setTextColor(RGB565_BLUE);
    g->setCursor(5, 4);
    g->print("Battery");
    g->setTextSize(1);
    g->setTextColor(s_connected ? RGB565_GREEN : RGB565_RED);
    g->setCursor(100, 10);
    g->print(s_connected ? "OK" : "N/C");
    if (s_connected && s_info.sealed) {
        g->setTextColor(RGB565_ORANGE);
        g->setCursor(125, 10);
        g->print("SEAL");
    }
    g->drawFastHLine(0, 24, LCD_WIDTH, RGB565_DARKGREY);
}

static void drawFooter() {
    auto *g = Display::gfx();
    g->setTextSize(1);
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(3, LCD_HEIGHT - 12);
    g->print("Clk=page Dbl=back Hld=act");
}

static void drawInfo() {
    auto *g = Display::gfx();
    g->fillScreen(RGB565_BLACK);
    drawHeader();

    int y = 28;
    g->setTextSize(1);

    if (!s_connected) {
        g->setTextColor(RGB565_RED);
        g->setCursor(5, 50);  g->print("No battery detected");
        g->setCursor(5, 66);  g->print("Connect SDA/SCL/GND");
        g->setCursor(5, 82);  g->printf("SDA=%d SCL=%d", BATT_SDA, BATT_SCL);
        g->setCursor(5, 98);  g->print("Addr: 0x0B");
        g->setTextColor(RGB565_YELLOW);
        g->setCursor(5, 120); g->print("Hold = rescan");
        drawFooter();
        return;
    }

    // SOC
    g->setTextSize(3);
    g->setTextColor(RGB565_WHITE);
    g->setCursor(15, y);
    g->printf("%d%%", s_info.stateOfCharge);
    y += 32;

    g->setTextSize(1);
    g->setTextColor(RGB565_CYAN);
    g->setCursor(5, y);
    g->printf("%.2fV  %dmA  %.1fC",
        s_info.voltage_mV / 1000.0f,
        s_info.current_mA,
        s_info.temperature_C);
    y += 14;

    g->setTextColor(RGB565_WHITE);
    g->setCursor(5, y);
    g->printf("Cap: %d/%d mAh", s_info.remainCapacity_mAh, s_info.fullCapacity_mAh);
    y += 12;
    g->setCursor(5, y);
    g->printf("Design: %d mAh", s_info.designCapacity_mAh);
    y += 12;

    g->setTextColor(RGB565_YELLOW);
    g->setCursor(5, y);
    g->printf("Cycles: %d", s_info.cycleCount);
    y += 16;

    // Model
    g->setTextColor(RGB565_MAGENTA);
    g->setCursor(5, y);
    g->printf("Model: %s", DJIBattery::modelName(s_info.model));
    y += 14;

    if (DJIBattery::modelNeedsDjiKey(s_info.model)) {
        g->setTextColor(RGB565_RED);
        g->setCursor(5, y);
        g->print("! Needs DJI key");
        y += 14;
    }

    // PF warning
    if (s_info.hasPF) {
        g->setTextColor(RGB565_RED);
        g->setCursor(5, y);
        g->print("!! PERMANENT FAIL !!");
        y += 14;
    }

    // Capacity bar
    int barY = LCD_HEIGHT - 28;
    int barW = LCD_WIDTH - 20;
    int barH = 10;
    g->drawRect(9, barY, barW + 2, barH + 2, RGB565_DARKGREY);
    int fillW = s_info.stateOfCharge * barW / 100;
    uint16_t barCol = RGB565_GREEN;
    if (s_info.stateOfCharge < 20) barCol = RGB565_RED;
    else if (s_info.stateOfCharge < 50) barCol = RGB565_YELLOW;
    g->fillRect(10, barY + 1, fillW, barH, barCol);

    drawFooter();
}

static void drawCells() {
    auto *g = Display::gfx();
    g->fillScreen(RGB565_BLACK);
    drawHeader();

    int y = 28;
    g->setTextSize(1);
    g->setTextColor(RGB565_WHITE);
    g->setCursor(5, y);
    g->printf("Cells (%s):", s_info.daStatus1Valid ? "sync" : "async");
    y += 14;

    if (!s_connected) { drawFooter(); return; }

    // Use synced cells if available, else async
    uint16_t cells[4];
    for (int i = 0; i < 4; i++) {
        cells[i] = s_info.daStatus1Valid ? s_info.cellVoltSync[i] : s_info.cellVoltage[i];
    }

    int barW = LCD_WIDTH - 55;
    for (int i = 0; i < 4; i++) {
        uint16_t mv = cells[i];
        if (mv == 0xFFFF || mv == 0) continue;

        float v = mv / 1000.0f;
        g->setTextSize(1);
        g->setTextColor(RGB565_WHITE);
        g->setCursor(5, y);
        g->printf("C%d %.3fV", i + 1, v);

        int barY = y + 12;
        int barH = 10;
        g->drawRect(4, barY, barW + 2, barH + 2, RGB565_DARKGREY);
        float pct = constrain((v - 2.5f) / (4.2f - 2.5f), 0.0f, 1.0f);
        int fillW = pct * barW;
        uint16_t col = RGB565_GREEN;
        if (v < 3.3f) col = RGB565_RED;
        else if (v < 3.6f) col = RGB565_YELLOW;
        g->fillRect(5, barY + 1, fillW, barH, col);

        y += 28;
    }

    // Delta
    uint16_t minV = 5000, maxV = 0;
    for (int i = 0; i < 4; i++) {
        if (cells[i] != 0xFFFF && cells[i] > 0) {
            if (cells[i] < minV) minV = cells[i];
            if (cells[i] > maxV) maxV = cells[i];
        }
    }
    if (maxV > minV) {
        uint16_t delta = maxV - minV;
        g->setTextColor(delta > 50 ? RGB565_RED : RGB565_GREEN);
        g->setCursor(5, y + 6);
        g->printf("Delta: %dmV %s", delta, delta > 50 ? "UNBALANCED" : "OK");
    }

    // Pack voltage (if sync available)
    if (s_info.daStatus1Valid) {
        g->setTextColor(RGB565_CYAN);
        g->setCursor(5, y + 22);
        g->printf("Pack: %.2fV (sync)", s_info.packVoltage / 1000.0f);
    }

    drawFooter();
}

static void drawStatus() {
    auto *g = Display::gfx();
    g->fillScreen(RGB565_BLACK);
    drawHeader();

    int y = 28;
    g->setTextSize(1);

    if (!s_connected) { drawFooter(); return; }

    // Operation status (seal state + flags)
    g->setTextColor(RGB565_CYAN);
    g->setCursor(5, y);
    g->printf("OpStat 0x%08X", s_info.operationStatus);
    y += 12;
    g->setTextColor(s_info.sealed ? RGB565_ORANGE : RGB565_GREEN);
    g->setCursor(5, y);
    g->print(DJIBattery::decodeOperationStatus(s_info.operationStatus));
    y += 16;

    // Safety status
    g->setTextColor(RGB565_CYAN);
    g->setCursor(5, y);
    g->printf("SafStat 0x%08X", s_info.safetyStatus);
    y += 12;
    bool hasSafety = (s_info.safetyStatus != 0 && s_info.safetyStatus != 0xFFFFFFFF);
    g->setTextColor(hasSafety ? RGB565_YELLOW : RGB565_DARKGREY);
    g->setCursor(5, y);
    String ss = DJIBattery::decodeSafetyStatus(s_info.safetyStatus);
    if (ss.length() > 28) ss = ss.substring(0, 28);
    g->print(ss);
    y += 16;

    // PF status (critical)
    g->setTextColor(RGB565_CYAN);
    g->setCursor(5, y);
    g->printf("PFStat  0x%08X", s_info.pfStatus);
    y += 12;
    g->setTextColor(s_info.hasPF ? RGB565_RED : RGB565_GREEN);
    g->setCursor(5, y);
    String pf = DJIBattery::decodePFStatus(s_info.pfStatus);
    if (pf.length() > 28) pf = pf.substring(0, 28);
    g->print(pf);
    y += 16;

    // Manufacturing status
    g->setTextColor(RGB565_CYAN);
    g->setCursor(5, y);
    g->printf("MfgStat 0x%08X", s_info.manufacturingStatus);
    y += 12;
    g->setTextColor(RGB565_WHITE);
    g->setCursor(5, y);
    String ms = DJIBattery::decodeManufacturingStatus(s_info.manufacturingStatus);
    if (ms.length() > 28) ms = ms.substring(0, 28);
    g->print(ms);

    drawFooter();
}

static void drawLifetime() {
    auto *g = Display::gfx();
    g->fillScreen(RGB565_BLACK);
    drawHeader();

    int y = 28;
    g->setTextSize(1);

    if (!s_connected) { drawFooter(); return; }

    g->setTextColor(RGB565_WHITE);
    g->setCursor(5, y); g->printf("Mfr: %s", s_info.manufacturerName.c_str()); y += 12;
    g->setCursor(5, y); g->printf("Dev: %s", s_info.deviceName.c_str()); y += 12;
    g->setCursor(5, y); g->printf("Chem: %s", s_info.chemistry.c_str()); y += 12;
    g->setCursor(5, y); g->printf("S/N: %d", s_info.serialNumber); y += 12;

    // Decode manufacture date (DJI format: YYYY*512 + MONTH*32 + DAY)
    uint16_t d = s_info.manufactureDate;
    int year = ((d >> 9) & 0x7F) + 1980;
    int mon = (d >> 5) & 0x0F;
    int day = d & 0x1F;
    g->setCursor(5, y); g->printf("Date: %04d-%02d-%02d", year, mon, day); y += 16;

    g->setTextColor(RGB565_MAGENTA);
    g->setCursor(5, y); g->printf("Model: %s", DJIBattery::modelName(s_info.model)); y += 14;

    g->setTextColor(RGB565_CYAN);
    g->setCursor(5, y); g->printf("Chip: 0x%04X", s_info.chipType); y += 12;
    if (s_info.chipType == 0x4307)      { g->setCursor(5, y); g->print("(BQ40z307)"); }
    else if (s_info.chipType == 0x0550) { g->setCursor(5, y); g->print("(BQ30z55)"); }
    y += 12;

    g->setCursor(5, y); g->printf("FW: 0x%04X", s_info.firmwareVersion); y += 12;
    g->setCursor(5, y); g->printf("HW: 0x%04X", s_info.hardwareVersion); y += 12;

    y += 8;
    if (DJIBattery::modelNeedsDjiKey(s_info.model)) {
        g->setTextColor(RGB565_RED);
        g->setCursor(5, y); g->print("! Service unavailable");
        y += 12;
        g->setCursor(5, y); g->print("  Needs DJI per-pack");
        y += 12;
        g->setCursor(5, y); g->print("  key (not in OSS)");
    } else {
        g->setTextColor(RGB565_GREEN);
        g->setCursor(5, y); g->print("Service available");
        y += 12;
        g->setTextColor(RGB565_DARKGREY);
        g->setCursor(5, y); g->print("(unseal + clear PF)");
    }

    drawFooter();
}

static void drawService() {
    auto *g = Display::gfx();
    g->fillScreen(RGB565_BLACK);
    drawHeader();

    int y = 28;
    g->setTextSize(1);

    g->setTextColor(RGB565_RED);
    g->setCursor(5, y); g->print("!! SERVICE MODE !!");
    y += 14;

    g->setTextColor(RGB565_YELLOW);
    g->setCursor(5, y); g->print("Hold = unseal & clear PF");
    y += 14;
    g->setCursor(5, y); g->print("(full sequence)");
    y += 18;

    g->setTextColor(RGB565_WHITE);
    g->setCursor(5, y); g->printf("Seal state: %s",
        s_info.sealed ? "SEALED" : "UNSEALED");
    y += 14;
    g->setCursor(5, y); g->printf("PF status: %s",
        s_info.hasPF ? "HAS PF" : "OK");
    y += 14;

    if (DJIBattery::modelNeedsDjiKey(s_info.model)) {
        g->setTextColor(RGB565_RED);
        g->setCursor(5, y);
        g->print("Model needs DJI key!");
        y += 14;
        g->setCursor(5, y);
        g->print("Will try but likely fail");
        y += 14;
    }

    g->setTextColor(RGB565_DARKGREY);
    y += 8;
    g->setCursor(5, y); g->print("Sequence:");
    y += 12;
    g->setCursor(5, y); g->print(" 1. unseal (try keys)");
    y += 11;
    g->setCursor(5, y); g->print(" 2. MAC 0x29 PF reset");
    y += 11;
    g->setCursor(5, y); g->print(" 3. MAC 0x54 clear");
    y += 11;
    g->setCursor(5, y); g->print(" 4. MAC 0x41 reset");
    y += 11;
    g->setCursor(5, y); g->print(" 5. seal");

    drawFooter();
}

static void drawScan() {
    auto *g = Display::gfx();
    g->fillScreen(RGB565_BLACK);
    drawHeader();

    g->setTextSize(1);
    g->setTextColor(RGB565_WHITE);
    g->setCursor(5, 30);
    g->print("I2C Bus Scanner");
    g->setCursor(5, 44);
    g->printf("SDA=%d SCL=%d", BATT_SDA, BATT_SCL);
    g->setCursor(5, 60);
    g->setTextColor(RGB565_YELLOW);
    g->print("Hold = start scan");
    drawFooter();
}

static void runScan() {
    auto *g = Display::gfx();
    g->fillRect(0, 75, LCD_WIDTH, 200, RGB565_BLACK);
    g->setTextSize(1);
    g->setTextColor(RGB565_CYAN);
    g->setCursor(5, 75);
    g->print("Scanning...");

    int y = 90;
    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            g->setCursor(5, y);
            g->setTextColor(RGB565_GREEN);
            g->printf("0x%02X", addr);
            g->setTextColor(RGB565_WHITE);
            if (addr == 0x0B) g->print(" DJI Battery");
            else if (addr == 0x6B) g->print(" QMI8658 IMU");
            else if (addr == 0x6A) g->print(" QMI8658 alt");
            else g->print(" unknown");
            y += 14;
            found++;
        }
    }
    g->setCursor(5, y + 10);
    g->setTextColor(RGB565_DARKGREY);
    g->printf("Found %d device(s)", found);
}

// Confirm dialog: shows warning, requires Hold to proceed, DblClk to cancel
// Returns true if user confirmed, false if cancelled
static bool confirmDanger(const char* title, const char* line1, const char* line2 = nullptr) {
    auto *g = Display::gfx();
    g->fillScreen(RGB565_BLACK);

    // Warning icon area
    g->fillRect(0, 0, LCD_WIDTH, 30, RGB565_MAROON);
    g->setTextSize(2);
    g->setTextColor(RGB565_WHITE);
    g->setCursor(10, 7);
    g->print("!! CONFIRM");

    g->setTextSize(1);
    g->setTextColor(RGB565_YELLOW);
    g->setCursor(5, 40);
    g->print(title);

    g->setTextColor(RGB565_WHITE);
    g->setCursor(5, 60);
    g->print(line1);
    if (line2) {
        g->setCursor(5, 72);
        g->print(line2);
    }

    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(5, 100);
    g->print("Make sure battery V>3.3/cell");
    g->setCursor(5, 114);
    g->print("and delta <50mV");

    // Big instruction
    g->setTextSize(2);
    g->setTextColor(RGB565_GREEN);
    g->setCursor(5, 150);
    g->print("Hold = OK");
    g->setTextColor(RGB565_RED);
    g->setCursor(5, 180);
    g->print("DblClk=cancel");
    g->setCursor(5, 210);
    g->setTextSize(1);
    g->setTextColor(RGB565_DARKGREY);
    g->print("Click = ignore");

    while (true) {
        feed_wdt();
        StatusLed::loop();
        ButtonEvent evt = Button::poll();
        if (evt == BTN_LONG_PRESS) return true;
        if (evt == BTN_DOUBLE_CLICK) return false;
        delay(10);
    }
}

// Simple progress bar (x/xmax) + optional text
static void drawProgress(int step, int total, const char* label) {
    auto *g = Display::gfx();
    // Progress bar at bottom
    int y = LCD_HEIGHT - 40;
    g->fillRect(0, y, LCD_WIDTH, 30, RGB565_BLACK);

    g->setTextSize(1);
    g->setTextColor(RGB565_CYAN);
    g->setCursor(5, y);
    g->printf("[%d/%d] %s", step, total, label);

    int barW = LCD_WIDTH - 20;
    int barY = y + 14;
    g->drawRect(9, barY, barW + 2, 10, RGB565_DARKGREY);
    int fillW = step * barW / total;
    g->fillRect(10, barY + 1, fillW, 8, RGB565_GREEN);
}

// Full service sequence: unseal + clear PF + reseal
static void runFullService() {
    auto *g = Display::gfx();
    g->fillScreen(RGB565_BLACK);
    drawHeader();

    int y = 30;
    g->setTextSize(1);

    drawProgress(1, 5, "Unsealing");
    g->setTextColor(RGB565_YELLOW);
    g->setCursor(5, y); g->print("[1/5] Unsealing..."); y += 14;
    UnsealResult ur = DJIBattery::unseal();

    g->setCursor(5, y);
    if (ur == UNSEAL_OK) {
        g->setTextColor(RGB565_GREEN);
        g->print("  OK");
    } else if (ur == UNSEAL_UNSUPPORTED_MODEL) {
        g->setTextColor(RGB565_RED);
        g->print("  NO KEY (Mavic 3/4?)");
        goto end;
    } else if (ur == UNSEAL_REJECTED_SEALED) {
        g->setTextColor(RGB565_RED);
        g->print("  REJECTED");
        goto end;
    } else {
        g->setTextColor(RGB565_RED);
        g->print("  NO RESPONSE");
        goto end;
    }
    y += 14;

    g->setTextColor(RGB565_YELLOW);
    drawProgress(2, 5, "MAC 0x29 PF reset");
    g->setCursor(5, y); g->print("[2/5] MAC 0x29 PF reset"); y += 14;
    drawProgress(3, 5, "MAC 0x54 clear");
    g->setCursor(5, y); g->print("[3/5] MAC 0x54 clear"); y += 14;
    drawProgress(4, 5, "Verify + reset");
    g->setCursor(5, y); g->print("[4/5] Verify + reset"); y += 14;

    if (DJIBattery::clearPFProper()) {
        g->setTextColor(RGB565_GREEN);
        g->setCursor(5, y); g->print("  Cleared!"); y += 14;
    } else {
        g->setTextColor(RGB565_RED);
        g->setCursor(5, y); g->print("  FAILED"); y += 14;
    }

    g->setTextColor(RGB565_YELLOW);
    drawProgress(5, 5, "Sealing");
    g->setCursor(5, y); g->print("[5/5] Sealing..."); y += 14;
    DJIBattery::seal();
    g->setTextColor(RGB565_GREEN);
    g->setCursor(5, y); g->print("  Done");

end:
    y += 20;
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(5, y); g->print("Click = back");
    while (Button::poll() != BTN_CLICK) delay(10);
}

static void redrawPage() {
    switch (s_page) {
        case BP_INFO:     drawInfo(); break;
        case BP_CELLS:    drawCells(); break;
        case BP_STATUS:   drawStatus(); break;
        case BP_LIFETIME: drawLifetime(); break;
        case BP_SERVICE:  drawService(); break;
        case BP_SCAN:     drawScan(); break;
        default: break;
    }
}

void runBatteryTool() {
    s_page = BP_INFO;
    DJIBattery::init();
    s_connected = DJIBattery::isConnected();
    if (s_connected) s_info = DJIBattery::readAll();

    redrawPage();

    uint32_t lastRefresh = 0;

    while (true) {
        feed_wdt();
        StatusLed::loop();
        ButtonEvent evt = Button::poll();

        if (evt == BTN_CLICK) {
            s_page = (BattPage)((s_page + 1) % BP_COUNT);
            redrawPage();
        } else if (evt == BTN_DOUBLE_CLICK) {
            return;
        } else if (evt == BTN_LONG_PRESS) {
            if (s_page == BP_SCAN) {
                runScan();
            } else if (s_page == BP_SERVICE && s_connected) {
                // Require explicit confirmation before dangerous ops
                if (confirmDanger("Full Service", "Unseal + Clear PF + Seal",
                                  DJIBattery::modelNeedsDjiKey(s_info.model) ?
                                      "May fail: Mavic 3/4 needs DJI key" :
                                      "TI default key will be tried")) {
                    runFullService();
                    s_info = DJIBattery::readAll();
                }
                redrawPage();
            } else {
                // Refresh
                s_connected = DJIBattery::isConnected();
                if (s_connected) s_info = DJIBattery::readAll();
                redrawPage();
            }
        }

        // Auto-refresh on info/cells/status pages
        if ((s_page == BP_INFO || s_page == BP_CELLS || s_page == BP_STATUS) &&
            (millis() - lastRefresh > 2000)) {
            s_connected = DJIBattery::isConnected();
            if (s_connected) s_info = DJIBattery::readAll();
            redrawPage();
            lastRefresh = millis();
        }

        delay(10);
    }
}
