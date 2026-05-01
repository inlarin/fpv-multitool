#include "wifi_manager.h"
#include <WiFi.h>
#include <Preferences.h>
#include "board_settings.h"

static WifiManager::Mode s_mode = WifiManager::MODE_OFF;
static String s_ssid;

bool WifiManager::startAP(const char* ssid, const char* pass) {
    stop();
    WiFi.mode(WIFI_AP);
    bool ok = WiFi.softAP(ssid, pass);
    if (ok) {
        s_mode = MODE_AP;
        s_ssid = ssid;
        Serial.printf("[WiFi] AP started: %s, IP: %s\n", ssid, WiFi.softAPIP().toString().c_str());
    } else {
        Serial.println("[WiFi] AP start FAILED");
    }
    return ok;
}

bool WifiManager::startSTA(const char* ssid, const char* pass, uint32_t timeoutMs) {
    stop();
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start < timeoutMs)) {
        delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
        s_mode = MODE_STA;
        s_ssid = ssid;
        Serial.printf("[WiFi] STA connected: %s, IP: %s\n", ssid, WiFi.localIP().toString().c_str());
        return true;
    } else {
        Serial.printf("[WiFi] STA connect failed to %s\n", ssid);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        s_mode = MODE_OFF;
        return false;
    }
}

void WifiManager::stop() {
    if (s_mode == MODE_AP) WiFi.softAPdisconnect(true);
    else if (s_mode == MODE_STA) WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    s_mode = MODE_OFF;
    s_ssid = "";
}

WifiManager::Mode WifiManager::currentMode() { return s_mode; }

String WifiManager::getIP() {
    if (s_mode == MODE_AP) return WiFi.softAPIP().toString();
    if (s_mode == MODE_STA) return WiFi.localIP().toString();
    return "";
}

String WifiManager::getSSID() { return s_ssid; }

int WifiManager::getRSSI() {
    return (s_mode == MODE_STA) ? WiFi.RSSI() : 0;
}

int WifiManager::clientCount() {
    return (s_mode == MODE_AP) ? WiFi.softAPgetStationNum() : 0;
}

// Credentials live in BoardSettings (NVS namespace 'boardcfg', keys
// 'wifi_ssid' / 'wifi_pass'). Both surfaces -- web /api/wifi/save
// (calls WifiManager::saveCredentials) and SC01 LVGL Settings (calls
// BoardSettings::setWifi directly) -- now write to the same place,
// so a save from either side is honoured by the next boot's
// autoStartWifi.
//
// One-shot migration handles devices that already had creds in the
// legacy 'wifi' namespace from before this commit: on first
// loadCredentials call we copy them into boardcfg + wipe the old
// namespace.

static void migrateLegacyCreds() {
    // Already migrated? Skip.
    if (BoardSettings::wifiSsid().length() > 0) return;

    Preferences legacy;
    if (!legacy.begin("wifi", true)) return;
    String old_ssid = legacy.getString("ssid", "");
    String old_pass = legacy.getString("pass", "");
    legacy.end();
    if (old_ssid.length() == 0) return;

    Serial.printf("[WiFi] migrating legacy creds for SSID '%s' to boardcfg\n",
                  old_ssid.c_str());
    BoardSettings::setWifi(old_ssid, old_pass);

    Preferences wipe;
    if (wipe.begin("wifi", false)) {
        wipe.clear();
        wipe.end();
    }
}

bool WifiManager::saveCredentials(const char* ssid, const char* pass) {
    BoardSettings::setWifi(String(ssid ? ssid : ""),
                           String(pass ? pass : ""));
    return true;
}

bool WifiManager::loadCredentials(String &ssid, String &pass) {
    migrateLegacyCreds();
    ssid = BoardSettings::wifiSsid();
    pass = BoardSettings::wifiPass();
    return ssid.length() > 0;
}

void WifiManager::clearCredentials() {
    BoardSettings::setWifi(String(""), String(""));
    // Also wipe the legacy namespace if it somehow still has values.
    Preferences legacy;
    if (legacy.begin("wifi", false)) {
        legacy.clear();
        legacy.end();
    }
}
