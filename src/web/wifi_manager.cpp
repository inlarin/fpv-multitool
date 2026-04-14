#include "wifi_manager.h"
#include <WiFi.h>
#include <Preferences.h>

static WifiManager::Mode s_mode = WifiManager::MODE_OFF;
static String s_ssid;
static Preferences s_prefs;

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

bool WifiManager::saveCredentials(const char* ssid, const char* pass) {
    s_prefs.begin("wifi", false);
    s_prefs.putString("ssid", ssid);
    s_prefs.putString("pass", pass);
    s_prefs.end();
    return true;
}

bool WifiManager::loadCredentials(String &ssid, String &pass) {
    s_prefs.begin("wifi", true);
    ssid = s_prefs.getString("ssid", "");
    pass = s_prefs.getString("pass", "");
    s_prefs.end();
    return ssid.length() > 0;
}

void WifiManager::clearCredentials() {
    s_prefs.begin("wifi", false);
    s_prefs.clear();
    s_prefs.end();
}
