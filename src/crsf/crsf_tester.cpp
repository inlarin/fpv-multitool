#include "crsf_tester.h"
#include <Arduino.h>
#include "pin_config.h"
#include "ui/display.h"
#include "ui/button.h"
#include "ui/status_led.h"
#include "wdt.h"
#include "crsf_service.h"
#include "web/web_state.h"
#include "core/pin_port.h"

// Live CRSF telemetry viewer.
// Pages: LINK  |  BATTERY  |  CHANNELS (AETR)
// Click = next page, DblClick = prev, Hold = exit.

enum CrsfPage { PG_LINK, PG_BATT, PG_CHANS, PG_COUNT };

static const char* pageName(CrsfPage p) {
    switch (p) {
        case PG_LINK:  return "Link";
        case PG_BATT:  return "Battery";
        case PG_CHANS: return "Channels";
        default:       return "?";
    }
}

static bool s_startedHere = false;  // true if we started CRSFService in this session

static void ensureRunning() {
    if (CRSFService::isRunning()) return;
    if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "crsf")) {
        Serial.println("[CRSF] Port B busy — switch to UART in System → Port B Mode");
        return;
    }
    bool inv = false;
    { WebState::Lock lock; inv = WebState::crsf.inverted; }
    CRSFService::begin(&Serial1,
                       PinPort::rx_pin(PinPort::PORT_B),
                       PinPort::tx_pin(PinPort::PORT_B),
                       420000, inv);
    { WebState::Lock lock; WebState::crsf.enabled = true; }
    s_startedHere = true;
}

static void drawHeader(const CRSFService::State& st, CrsfPage page) {
    auto *g = Display::gfx();
    uint16_t bg = st.connected ? RGB565_DARKGREEN : RGB565_MAROON;
    g->fillRect(0, 0, LCD_WIDTH, 22, bg);
    g->setTextSize(1);
    g->setTextColor(RGB565_WHITE);
    g->setCursor(5, 7);
    g->print(st.connected ? "LINK UP" : "NO SIGNAL");

    // Page indicator on the right
    char buf[16];
    snprintf(buf, sizeof(buf), "%s", pageName(page));
    int tw = (int)strlen(buf) * 6;
    g->setCursor(LCD_WIDTH - tw - 5, 7);
    g->print(buf);
}

static void drawLinkPage(const CRSFService::State& st) {
    auto *g = Display::gfx();
    const auto &l = st.link;

    g->setTextSize(1);
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(5, 30);
    g->printf("Frames: %lu  BadCRC: %lu",
              (unsigned long)st.total_frames,
              (unsigned long)st.bad_crc);

    if (!l.valid) {
        g->setTextColor(RGB565_DARKGREY);
        g->setCursor(5, 60);
        g->print("Waiting for LinkStats...");
        return;
    }

    // Big LQ number
    g->setTextSize(4);
    int lq = l.uplink_link_quality;
    uint16_t lqColor = (lq >= 90) ? RGB565_GREEN :
                       (lq >= 70) ? RGB565_YELLOW : RGB565_RED;
    g->setTextColor(lqColor);
    g->setCursor(20, 50);
    g->printf("%3d%%", lq);

    g->setTextSize(1);
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(20, 90);
    g->print("Uplink LQ");

    // RSSI & SNR
    g->setTextSize(2);
    g->setTextColor(RGB565_WHITE);
    g->setCursor(5, 115);
    g->printf("RSSI: -%d", l.uplink_rssi1);
    g->setCursor(5, 140);
    g->printf("SNR : %d", (int)l.uplink_snr);

    // RF mode & TX power
    g->setTextSize(1);
    g->setTextColor(RGB565_CYAN);
    g->setCursor(5, 170);
    g->printf("RF mode: %u", l.rf_mode);
    g->setCursor(5, 185);
    g->printf("TX pwr : %u", l.uplink_tx_power);
    g->setCursor(5, 200);
    g->printf("Antenna: %u", l.active_antenna);

    // Downlink summary
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(5, 220);
    g->printf("DL LQ:%u  RSSI:-%u  SNR:%d",
              l.downlink_link_quality,
              l.downlink_rssi,
              (int)l.downlink_snr);
}

static void drawBatteryPage(const CRSFService::State& st) {
    auto *g = Display::gfx();
    const auto &b = st.battery;

    if (!b.valid) {
        g->setTextSize(1);
        g->setTextColor(RGB565_DARKGREY);
        g->setCursor(5, 60);
        g->print("No battery telem");
        return;
    }

    // Voltage big
    g->setTextSize(3);
    g->setTextColor(RGB565_WHITE);
    g->setCursor(10, 40);
    g->printf("%2u.%1uV", b.voltage_dV / 10, b.voltage_dV % 10);

    // Current
    g->setTextSize(2);
    g->setTextColor(RGB565_CYAN);
    g->setCursor(10, 85);
    g->printf("%u.%uA", b.current_dA / 10, b.current_dA % 10);

    // Capacity
    g->setCursor(10, 115);
    g->setTextColor(RGB565_DARKGREY);
    g->printf("%lumAh", (unsigned long)b.capacity_mAh);

    // Remaining %
    int pct = b.remaining_pct;
    uint16_t col = (pct > 40) ? RGB565_GREEN :
                   (pct > 15) ? RGB565_YELLOW : RGB565_RED;
    g->setTextSize(2);
    g->setTextColor(col);
    g->setCursor(10, 150);
    g->printf("%d%%", pct);

    // Simple bar
    int barY = 180;
    int barH = 14;
    int barW = LCD_WIDTH - 20;
    g->drawRect(9, barY - 1, barW + 2, barH + 2, RGB565_DARKGREY);
    int fillW = pct * barW / 100;
    if (fillW > 0) g->fillRect(10, barY, fillW, barH, col);
}

static void drawChannelsPage(const CRSFService::State& st) {
    auto *g = Display::gfx();
    const auto &c = st.channels;

    if (!c.valid) {
        g->setTextSize(1);
        g->setTextColor(RGB565_DARKGREY);
        g->setCursor(5, 60);
        g->print("No RC channels");
        return;
    }

    // AETR (1-4) as labeled bars. 172..1811, center 992.
    static const char* names[4] = {"A", "E", "T", "R"};
    int barW = LCD_WIDTH - 40;
    int y = 32;
    int rowH = 40;
    for (int i = 0; i < 4; i++) {
        int v = c.ch[i];
        if (v < 172) v = 172; if (v > 1811) v = 1811;
        int pct = (v - 172) * 100 / (1811 - 172);

        g->setTextSize(2);
        g->setTextColor(RGB565_CYAN);
        g->setCursor(5, y);
        g->print(names[i]);

        g->setTextSize(1);
        g->setTextColor(RGB565_DARKGREY);
        g->setCursor(25, y);
        g->printf("%4d", v);

        // Bar
        int bY = y + 16;
        int bH = 10;
        int bX = 25;
        int bW = barW;
        g->drawRect(bX - 1, bY - 1, bW + 2, bH + 2, RGB565_DARKGREY);
        int fill = pct * bW / 100;
        uint16_t col = RGB565_GREEN;
        g->fillRect(bX, bY, bW, bH, RGB565_BLACK);
        if (fill > 0) g->fillRect(bX, bY, fill, bH, col);

        // Center mark
        int cx = bX + bW / 2;
        g->drawFastVLine(cx, bY - 2, bH + 4, RGB565_YELLOW);

        y += rowH;
    }

    // Mode line
    if (st.mode.valid) {
        g->setTextSize(1);
        g->setTextColor(RGB565_ORANGE);
        g->setCursor(5, LCD_HEIGHT - 28);
        g->printf("Mode: %s", st.mode.name);
    }
}

static void drawFooter() {
    auto *g = Display::gfx();
    g->setTextSize(1);
    g->setTextColor(RGB565_DARKGREY);
    g->setCursor(3, LCD_HEIGHT - 12);
    g->print("Clk=next Dbl=prev Hld=exit");
}

static void drawAll(const CRSFService::State& st, CrsfPage page) {
    auto *g = Display::gfx();
    g->fillScreen(RGB565_BLACK);
    drawHeader(st, page);
    switch (page) {
        case PG_LINK:  drawLinkPage(st); break;
        case PG_BATT:  drawBatteryPage(st); break;
        case PG_CHANS: drawChannelsPage(st); break;
        default: break;
    }
    drawFooter();
}

void runCRSFTester() {
    s_startedHere = false;
    ensureRunning();

    CrsfPage page = PG_LINK;
    uint32_t lastDraw = 0;
    bool prevConnected = false;

    // Initial draw
    drawAll(CRSFService::state(), page);

    while (true) {
        feed_wdt();
        CRSFService::loop();  // pump CRSF UART

        StatusLed::loop();
        ButtonEvent evt = Button::poll();
        if (evt == BTN_CLICK) {
            page = (CrsfPage)((page + 1) % PG_COUNT);
            drawAll(CRSFService::state(), page);
        } else if (evt == BTN_DOUBLE_CLICK) {
            page = (CrsfPage)((page + PG_COUNT - 1) % PG_COUNT);
            drawAll(CRSFService::state(), page);
        } else if (evt == BTN_LONG_PRESS) {
            // Exit. Stop CRSF only if we started it in this session.
            if (s_startedHere) {
                CRSFService::end();
                PinPort::release(PinPort::PORT_B);
                WebState::Lock lock;
                WebState::crsf.enabled = false;
            }
            return;
        }

        // Periodic refresh (5 Hz) — telemetry updates are slow
        if (millis() - lastDraw > 200) {
            const auto &st = CRSFService::state();
            // Full redraw on connection state change (header color flips)
            if (st.connected != prevConnected) {
                drawAll(st, page);
                prevConnected = st.connected;
            } else {
                // Just repaint current page body (header is stable)
                auto *g = Display::gfx();
                g->fillRect(0, 22, LCD_WIDTH, LCD_HEIGHT - 22 - 14, RGB565_BLACK);
                switch (page) {
                    case PG_LINK:  drawLinkPage(st); break;
                    case PG_BATT:  drawBatteryPage(st); break;
                    case PG_CHANS: drawChannelsPage(st); break;
                    default: break;
                }
            }
            lastDraw = millis();
        }

        delay(2);
    }
}
