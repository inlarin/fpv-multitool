// Implementation: see status_screen.h.
//
// 172x320 portrait. Layout (y values are pixel rows):
//
//   y=  4  "FPV MultiTool"   (Montserrat 5x7 size 2 = 10x14, teal)
//   y= 22  "ESP32-S3-LCD-1.47B" (size 1, dark grey)
//   y= 38  ───────────────────  (horizontal rule)
//
//   y= 50  "WiFi  STA"             (label key + value)
//   y= 64  "IP    192.168.32.50"
//   y= 84  "Up    1m 30s"
//   y= 98  "Heap  140K free"
//   y=118  "USB   CDC"
//   y=132  "Port  IDLE"
//   y=152  "OTA   VALID"
//
//   y=300  "v0.30.0"  (right-aligned, dim grey)
//
// Refresh strategy: each value field has a fixed origin + clear-rect
// width; on tick, if a value changed, fillRect over old, draw new.
// No fillScreen anywhere after init() so there's zero flicker.

#include "status_screen.h"
#include "display.h"
#include "core/build_info.h"
#include "core/usb_mode.h"
#include "core/pin_port.h"

#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

namespace {

constexpr uint16_t COLOR_BG       = RGB565_BLACK;
constexpr uint16_t COLOR_TITLE    = RGB565(46, 134, 171);    // teal, matches SC01 LVGL accent
constexpr uint16_t COLOR_RULE     = RGB565(57, 65, 80);
constexpr uint16_t COLOR_LABEL    = RGB565(160, 160, 160);
constexpr uint16_t COLOR_VALUE    = RGB565(224, 224, 224);
constexpr uint16_t COLOR_OK       = RGB565(6, 167, 125);
constexpr uint16_t COLOR_WARN     = RGB565(230, 162, 60);
constexpr uint16_t COLOR_ERR      = RGB565(230, 57, 70);
constexpr uint16_t COLOR_DIM      = RGB565(108, 116, 128);

// Field offsets are picked so a 5x7 size-1 "label" of 4 chars fits
// before the value column at x=44.
constexpr int LABEL_X = 6;
constexpr int VALUE_X = 44;
constexpr int VALUE_W = LCD_WIDTH - VALUE_X - 4;   // pixels we may draw into

// Cached prior-render text per field, so we only redraw when changed.
// 32 chars covers any value we render at size 1.
struct Field {
    int      y;
    char     last[32];
    uint16_t last_color;
};
Field s_wifi   = { 50};
Field s_ip     = { 64};
Field s_up     = { 84};
Field s_heap   = { 98};
Field s_usb    = {118};
Field s_port   = {132};
Field s_ota    = {152};

uint32_t s_lastTickMs = 0;

// Draw a label:value row. Label colour is fixed (dim grey); value can
// vary (e.g. green for connected, red for AP, etc).
void drawValue(Field &f, const char *value, uint16_t color) {
    if (strcmp(f.last, value) == 0 && f.last_color == color) return;
    auto *g = Display::gfx();
    if (!g) return;
    g->fillRect(VALUE_X, f.y, VALUE_W, 8, COLOR_BG);
    g->setTextSize(1);
    g->setTextColor(color);
    g->setCursor(VALUE_X, f.y);
    g->print(value);
    strncpy(f.last, value, sizeof(f.last) - 1);
    f.last[sizeof(f.last) - 1] = '\0';
    f.last_color = color;
}

void drawLabel(int y, const char *label) {
    auto *g = Display::gfx();
    g->setTextSize(1);
    g->setTextColor(COLOR_LABEL);
    g->setCursor(LABEL_X, y);
    g->print(label);
}

void formatUptime(uint32_t s, char *out, size_t n) {
    if (s < 60)        snprintf(out, n, "%us", (unsigned)s);
    else if (s < 3600) snprintf(out, n, "%um %02us",
                                  (unsigned)(s / 60), (unsigned)(s % 60));
    else if (s < 86400) snprintf(out, n, "%uh %02um",
                                  (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
    else                snprintf(out, n, "%ud %02uh",
                                  (unsigned)(s / 86400), (unsigned)((s % 86400) / 3600));
}

const char *otaStateStr(esp_ota_img_states_t st) {
    switch (st) {
        case ESP_OTA_IMG_NEW:            return "NEW";
        case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING";
        case ESP_OTA_IMG_VALID:          return "VALID";
        case ESP_OTA_IMG_INVALID:        return "INVALID";
        case ESP_OTA_IMG_ABORTED:        return "ABORTED";
        case ESP_OTA_IMG_UNDEFINED:      return "UNDEF";
        default:                         return "?";
    }
}

esp_ota_img_states_t currentOtaState() {
    const esp_partition_t *p = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (p) esp_ota_get_state_partition(p, &st);
    return st;
}

} // namespace

void StatusScreen::init() {
    auto *g = Display::gfx();
    if (!g) return;

    g->fillScreen(COLOR_BG);

    // Title.
    g->setTextSize(2);
    g->setTextColor(COLOR_TITLE);
    g->setCursor(8, 4);
    g->print("FPV MultiTool");

    g->setTextSize(1);
    g->setTextColor(COLOR_DIM);
    g->setCursor(8, 22);
    g->print("ESP32-S3-LCD-1.47B");

    // Horizontal rule.
    g->drawFastHLine(0, 38, LCD_WIDTH, COLOR_RULE);

    // Static labels.
    drawLabel(s_wifi.y, "WiFi");
    drawLabel(s_ip.y,   "IP");
    drawLabel(s_up.y,   "Up");
    drawLabel(s_heap.y, "Heap");
    drawLabel(s_usb.y,  "USB");
    drawLabel(s_port.y, "Port");
    drawLabel(s_ota.y,  "OTA");

    // Version, bottom-right, dim grey, drawn once -- it doesn't change
    // at runtime, OTA installs a new image then reboots.
    g->setTextSize(1);
    g->setTextColor(COLOR_DIM);
    char vbuf[24];
    snprintf(vbuf, sizeof(vbuf), "%s", FW_VERSION);
    int xv = LCD_WIDTH - (int)(strlen(vbuf) * 6) - 4;
    g->setCursor(xv, LCD_HEIGHT - 12);
    g->print(vbuf);

    // Force initial value paint by clearing cache and ticking once.
    s_wifi.last[0] = s_ip.last[0] = s_up.last[0] = s_heap.last[0]
        = s_usb.last[0] = s_port.last[0] = s_ota.last[0] = '\0';
    s_lastTickMs = 0;
    StatusScreen::tick();
}

void StatusScreen::tick() {
    uint32_t now = millis();
    if (s_lastTickMs && (now - s_lastTickMs) < 1000) return;   // 1 Hz cap
    s_lastTickMs = now;

    auto *g = Display::gfx();
    if (!g) return;

    char buf[32];

    // ---- WiFi mode + IP ----
    wifi_mode_t wmode = WiFi.getMode();
    bool sta_connected = (WiFi.status() == WL_CONNECTED);
    if (sta_connected) {
        drawValue(s_wifi, "STA",                     COLOR_OK);
        drawValue(s_ip,   WiFi.localIP().toString().c_str(), COLOR_VALUE);
    } else if (wmode & WIFI_AP) {
        drawValue(s_wifi, "AP",                      COLOR_WARN);
        drawValue(s_ip,   WiFi.softAPIP().toString().c_str(), COLOR_VALUE);
    } else {
        drawValue(s_wifi, "off",                     COLOR_ERR);
        drawValue(s_ip,   "--",                      COLOR_DIM);
    }

    // ---- Uptime ----
    formatUptime(now / 1000, buf, sizeof(buf));
    drawValue(s_up, buf, COLOR_VALUE);

    // ---- Free heap ----
    snprintf(buf, sizeof(buf), "%uK free", (unsigned)(ESP.getFreeHeap() / 1024));
    drawValue(s_heap, buf, COLOR_VALUE);

    // ---- USB descriptor mode ----
    UsbDescriptorMode usb = UsbMode::active();
    const char *usb_str = UsbMode::name(usb);
    // strip parenthetical from "USB2TTL (UART bridge)" etc; just first word
    char usb_buf[16];
    {
        int i = 0;
        while (i < (int)sizeof(usb_buf) - 1 && usb_str[i] && usb_str[i] != ' ') {
            usb_buf[i] = usb_str[i]; i++;
        }
        usb_buf[i] = '\0';
    }
    drawValue(s_usb, usb_buf, COLOR_VALUE);

    // ---- Port B mode ----
    drawValue(s_port,
              PinPort::modeName(PinPort::currentMode(PinPort::PORT_B)),
              COLOR_VALUE);

    // ---- OTA state ----
    esp_ota_img_states_t st = currentOtaState();
    uint16_t ota_color = (st == ESP_OTA_IMG_VALID)          ? COLOR_OK
                       : (st == ESP_OTA_IMG_PENDING_VERIFY) ? COLOR_WARN
                                                            : COLOR_DIM;
    drawValue(s_ota, otaStateStr(st), ota_color);
}
