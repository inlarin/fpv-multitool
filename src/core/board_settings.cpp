#include "board_settings.h"
#include <Preferences.h>

namespace BoardSettings {

static Preferences s_prefs;
static bool        s_open = false;

static constexpr const char *NS_NAME       = "boardcfg";
static constexpr const char *KEY_ROTATION  = "rotation";
static constexpr const char *KEY_BRIGHT    = "brightness";
static constexpr const char *KEY_TOUCHCAL  = "touch_cal";   // bytes: uint16_t[8] = 16B
static constexpr const char *KEY_WIFI_SSID = "wifi_ssid";
static constexpr const char *KEY_WIFI_PASS = "wifi_pass";
static constexpr const char *KEY_BEACON_URL = "bcn_url";
static constexpr const char *KEY_BEACON_MS  = "bcn_ms";

void begin() {
    if (s_open) return;
    // false = read/write
    s_prefs.begin(NS_NAME, false);
    s_open = true;
}

uint8_t rotation() {
    if (!s_open) return 0;
    uint8_t r = s_prefs.getUChar(KEY_ROTATION, 0);
    return r > 3 ? 0 : r;
}

void setRotation(uint8_t rot) {
    if (!s_open) return;
    if (rot > 3) rot = 0;
    s_prefs.putUChar(KEY_ROTATION, rot);
}

uint8_t brightness() {
    if (!s_open) return 255;
    return s_prefs.getUChar(KEY_BRIGHT, 255);
}

void setBrightness(uint8_t val) {
    if (!s_open) return;
    s_prefs.putUChar(KEY_BRIGHT, val);
}

bool getTouchCalibrate(uint16_t out[8]) {
    if (!s_open) return false;
    size_t got = s_prefs.getBytes(KEY_TOUCHCAL, out, sizeof(uint16_t) * 8);
    return got == sizeof(uint16_t) * 8;
}

void setTouchCalibrate(const uint16_t in[8]) {
    if (!s_open) return;
    s_prefs.putBytes(KEY_TOUCHCAL, in, sizeof(uint16_t) * 8);
}

void clearTouchCalibrate() {
    if (!s_open) return;
    s_prefs.remove(KEY_TOUCHCAL);
}

String wifiSsid() {
    if (!s_open) return String();
    return s_prefs.getString(KEY_WIFI_SSID, "");
}

String wifiPass() {
    if (!s_open) return String();
    return s_prefs.getString(KEY_WIFI_PASS, "");
}

void setWifi(const String &ssid, const String &pass) {
    if (!s_open) return;
    s_prefs.putString(KEY_WIFI_SSID, ssid);
    s_prefs.putString(KEY_WIFI_PASS, pass);
}

bool hasWifiCreds() {
    return wifiSsid().length() > 0 && wifiPass().length() > 0;
}

String beaconUrl() {
    if (!s_open) return String();
    return s_prefs.getString(KEY_BEACON_URL, "");
}

uint32_t beaconIntervalMs() {
    if (!s_open) return 0;
    return s_prefs.getUInt(KEY_BEACON_MS, 0);
}

void setBeacon(const String &url, uint32_t interval_ms) {
    if (!s_open) return;
    s_prefs.putString(KEY_BEACON_URL, url);
    s_prefs.putUInt(KEY_BEACON_MS, interval_ms);
}

} // namespace BoardSettings
