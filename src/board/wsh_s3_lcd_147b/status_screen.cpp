// Implementation: see status_screen.h.
//
// Auto-rotation: on every tick we ask IMU::detectOrientation() what the
// LCD rotation should be given the current gravity vector. To avoid
// flicker on intermediate orientations (the user picking the device
// up, etc) we require the candidate rotation to be the same for two
// consecutive ticks (so ~2 s at 1 Hz) before applying.
//
// Layout adapts to rotation: landscape (320 wide) uses size-4 IP and
// size-2 status rows; portrait (172 wide) uses size-2 IP and size-1
// status rows because size-4 doesn't fit a 13+ char IP at 172 px.
//
// Refresh: per-field clear+redraw on tick (no fillScreen between
// rotation changes, so zero flicker in the steady state). On rotation
// change init() runs a full redraw of static chrome.

#include "status_screen.h"
#include "display.h"
#include "imu.h"
#include "safety.h"     // Safety::logf -> in-memory log ring (/api/sys/log)
#include "core/build_info.h"
#include "core/usb_mode.h"
#include "core/pin_port.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

namespace {

constexpr uint16_t COLOR_BG       = RGB565_BLACK;
constexpr uint16_t COLOR_TITLE    = RGB565(46, 134, 171);
constexpr uint16_t COLOR_RULE     = RGB565(57, 65, 80);
constexpr uint16_t COLOR_VALUE    = RGB565(224, 224, 224);
constexpr uint16_t COLOR_OK       = RGB565(6, 167, 125);
constexpr uint16_t COLOR_WARN     = RGB565(230, 162, 60);
constexpr uint16_t COLOR_ERR      = RGB565(230, 57, 70);
constexpr uint16_t COLOR_DIM      = RGB565(140, 145, 155);

inline int charW(int size) { return 6 * size; }
inline int charH(int size) { return 8 * size; }

// Cached prior-render text per field, so we only redraw when changed.
struct Field {
    int      x, y;
    int      size;
    int      max_chars;
    char     last[40];
    uint16_t last_color;
};

Field s_ip;
Field s_row1;
Field s_row2;
Field s_row3;

uint32_t s_lastTickMs = 0;

// Auto-rotate state machine: track the most recent IMU candidate and
// only apply a rotation change after the candidate stays the same
// for STABLE_TICKS consecutive ticks.
uint8_t  s_orient_candidate = 0xFF;
int      s_orient_stable    = 0;
constexpr int STABLE_TICKS = 2;     // ~2 s at 1 Hz tick

// True when the layout has been laid out for the currently-applied
// Display::rotation(). Cleared by markDirty() so the next tick redraws.
bool s_layout_drawn = false;

void resetField(Field &f, int x, int y, int size, int max_chars) {
    f.x = x; f.y = y; f.size = size; f.max_chars = max_chars;
    f.last[0] = '\0'; f.last_color = 0;
}

void layoutFor(int W, int H) {
    // Two regimes based on width.
    //   W >= 250 (landscape 320x172): big IP (size 4), big status (size 2)
    //   W <  250 (portrait 172x320):  smaller IP (size 2), small status (size 1)
    bool landscape = (W >= 250);

    if (landscape) {
        // y= 0..27   header (title + version)
        // y=38..69   IP size 4
        // y=82..   status rows size 2 (16 px tall, 6 px gap)
        resetField(s_ip,    8,  38, 4, 16);
        resetField(s_row1,  8,  82, 2, 32);
        resetField(s_row2,  8, 104, 2, 32);
        resetField(s_row3,  8, 126, 2, 32);
    } else {
        // 172 wide -- size-4 IP would be 312 px -> doesn't fit. Size 2 (12 px wide
        // per char) gets us 13*12=156 px -> fits 172 with margin. Status rows
        // drop to size 1 to fit 32 chars.
        resetField(s_ip,    6,  44, 2, 16);
        resetField(s_row1,  6,  72, 1, 32);
        resetField(s_row2,  6,  90, 1, 32);
        resetField(s_row3,  6, 108, 1, 32);
    }
}

void drawField(Field &f, const char *value, uint16_t color) {
    if (strcmp(f.last, value) == 0 && f.last_color == color) return;
    auto *g = Display::gfx();
    if (!g) return;
    g->fillRect(f.x, f.y, f.max_chars * charW(f.size), charH(f.size), COLOR_BG);
    g->setTextSize(f.size);
    g->setTextColor(color);
    g->setCursor(f.x, f.y);
    g->print(value);
    strncpy(f.last, value, sizeof(f.last) - 1);
    f.last[sizeof(f.last) - 1] = '\0';
    f.last_color = color;
}

void formatUptime(uint32_t s, char *out, size_t n) {
    if (s < 60)        snprintf(out, n, "%us", (unsigned)s);
    else if (s < 3600) snprintf(out, n, "%um%02us",
                                  (unsigned)(s / 60), (unsigned)(s % 60));
    else if (s < 86400) snprintf(out, n, "%uh%02um",
                                  (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
    else                snprintf(out, n, "%ud%02uh",
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

void drawStatic(int W, int H) {
    auto *g = Display::gfx();
    if (!g) return;

    g->fillScreen(COLOR_BG);

    bool landscape = (W >= 250);
    int title_size = landscape ? 2 : 1;
    int title_y    = 4;
    int rule_y     = landscape ? 28 : 18;
    int footer_y   = H - 10;

    // Title left.
    g->setTextSize(title_size);
    g->setTextColor(COLOR_TITLE);
    g->setCursor(landscape ? 8 : 4, title_y);
    g->print("FPV MultiTool");

    // Version right (always size 1).
    g->setTextSize(1);
    g->setTextColor(COLOR_DIM);
    int vw = (int)strlen(FW_VERSION) * charW(1);
    int vy = landscape ? 8 : (rule_y - 9);
    g->setCursor(W - vw - 4, vy);
    g->print(FW_VERSION);

    // Horizontal rule.
    g->drawFastHLine(0, rule_y, W, COLOR_RULE);

    // Footer (board codename).
    g->setTextSize(1);
    g->setTextColor(COLOR_DIM);
    g->setCursor(landscape ? 8 : 4, footer_y);
    if (landscape) {
        g->print("ESP32-S3-LCD-1.47B");
    } else {
        g->print("S3-LCD-1.47B");
    }
}

} // namespace

void StatusScreen::init() {
    auto *g = Display::gfx();
    if (!g) return;

    int W = g->width();
    int H = g->height();
    layoutFor(W, H);
    drawStatic(W, H);
    s_layout_drawn   = true;
    s_lastTickMs     = 0;

    StatusScreen::tick();
}

void StatusScreen::tick() {
    uint32_t now = millis();
    if (s_lastTickMs && (now - s_lastTickMs) < 1000) return;   // 1 Hz cap
    s_lastTickMs = now;

    auto *g = Display::gfx();
    if (!g) return;

    // ---- Auto-rotate from IMU ----
    // Log only on actual rotation changes -- per-tick accel logging
    // was useful during initial axis-mapping calibration but spams
    // /api/sys/log otherwise.
    if (IMU::isReady()) {
        uint8_t target = IMU::detectOrientation();
        if (target == s_orient_candidate) {
            if (s_orient_stable < 1000) s_orient_stable++;
        } else {
            s_orient_candidate = target;
            s_orient_stable    = 1;
        }
        if (s_orient_stable == STABLE_TICKS &&
            target != Display::rotation()) {
            Safety::logf("[imu] rotating %u -> %u", Display::rotation(), target);
            Display::setRotation(target);
            int W = g->width();
            int H = g->height();
            layoutFor(W, H);
            drawStatic(W, H);
        }
    }

    int W = g->width();

    // ---- Hero IP ----
    bool sta_connected = (WiFi.status() == WL_CONNECTED);
    wifi_mode_t wmode  = WiFi.getMode();
    if (sta_connected) {
        drawField(s_ip, WiFi.localIP().toString().c_str(), COLOR_OK);
    } else if (wmode & WIFI_AP) {
        drawField(s_ip, WiFi.softAPIP().toString().c_str(), COLOR_WARN);
    } else {
        drawField(s_ip, "no link",                          COLOR_ERR);
    }

    // ---- Row 1: mode + uptime + heap ----
    char buf[40];
    char up[16]; formatUptime(now / 1000, up, sizeof(up));
    const char *mode_str = sta_connected ? "STA"
                         : (wmode & WIFI_AP) ? "AP"
                                             : "--";
    if (W >= 250) {
        snprintf(buf, sizeof(buf), "%-3s  %-7s  %uK",
                 mode_str, up, (unsigned)(ESP.getFreeHeap() / 1024));
    } else {
        snprintf(buf, sizeof(buf), "%s %s %uK",
                 mode_str, up, (unsigned)(ESP.getFreeHeap() / 1024));
    }
    drawField(s_row1, buf, COLOR_VALUE);

    // ---- Row 2: USB + Port ----
    UsbDescriptorMode usb = UsbMode::active();
    const char *usb_full = UsbMode::name(usb);
    char usb_short[12] = {0};
    for (int i = 0; i < (int)sizeof(usb_short) - 1 && usb_full[i] && usb_full[i] != ' '; i++) {
        usb_short[i] = usb_full[i];
    }
    snprintf(buf, sizeof(buf), "USB:%s Port:%s",
             usb_short,
             PinPort::modeName(PinPort::currentMode(PinPort::PORT_B)));
    drawField(s_row2, buf, COLOR_VALUE);

    // ---- Row 3: OTA state ----
    esp_ota_img_states_t st = currentOtaState();
    uint16_t ota_color = (st == ESP_OTA_IMG_VALID)          ? COLOR_OK
                       : (st == ESP_OTA_IMG_PENDING_VERIFY) ? COLOR_WARN
                                                            : COLOR_DIM;
    snprintf(buf, sizeof(buf), "OTA:%s", otaStateStr(st));
    drawField(s_row3, buf, ota_color);
}
