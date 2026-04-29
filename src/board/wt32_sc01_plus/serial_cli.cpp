#include "serial_cli.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>

#include "board_settings.h"

namespace SerialCli {

// ---- reboot helpers ----------------------------------------------------------

// rebootToBootloader() WAS HERE. Removed deliberately for remote-deploy
// safety. Per ESP-IDF issue #13287 + esptool docs, the ESP32-S3
// USB-Serial-JTAG cannot exit download mode without an external reset
// (core-reset doesn't re-sample boot strapping pins). On a remote device
// without physical access this is a brick. If you ever need this path
// for bench debugging, do it via a physical BOOT+RST hold instead --
// don't bake an "easy" remote command for it.

[[noreturn]] static void rebootNormal() {
    Serial.println(F("[cli] reboot..."));
    Serial.flush();
    delay(100);
    esp_restart();
    while (true) { /* unreachable */ }
}

// ---- handlers ----------------------------------------------------------------

static void printHelp() {
    Serial.println(F("Commands:"));
    Serial.println(F("  reboot                  reset chip (normal)"));
    Serial.println(F("  wifi set <ssid> <pass>  save creds + reconnect"));
    Serial.println(F("  wifi show               print current ssid"));
    Serial.println(F("  wifi clear              wipe creds from NVS"));
    Serial.println(F("  wifi reconnect          bounce STA with current creds"));
    Serial.println(F("  help                    this list"));
}

static bool wifiReconnect(const String &ssid, const String &pass, uint32_t timeout_ms) {
    WiFi.mode(WIFI_STA);
    if (WiFi.getMode() != WIFI_OFF) WiFi.disconnect(true, true);
    delay(50);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeout_ms) {
        delay(250);
        if (WiFi.status() == WL_NO_SSID_AVAIL || WiFi.status() == WL_CONNECT_FAILED) break;
    }
    return WiFi.status() == WL_CONNECTED;
}

static void handle(String line) {
    line.trim();
    if (line.length() == 0) return;

    if (line.equalsIgnoreCase("help")) {
        printHelp();
        return;
    }
    if (line.equalsIgnoreCase("reboot")) {
        rebootNormal();
    }
    if (line.equalsIgnoreCase("wifi show")) {
        Serial.printf("ssid=\"%s\"  pass_set=%s\n",
                      BoardSettings::wifiSsid().c_str(),
                      BoardSettings::wifiPass().length() ? "true" : "false");
        return;
    }
    if (line.equalsIgnoreCase("wifi clear")) {
        BoardSettings::setWifi(String(""), String(""));
        Serial.println(F("creds cleared"));
        return;
    }
    if (line.equalsIgnoreCase("wifi reconnect")) {
        Serial.println(F("reconnecting..."));
        bool ok = wifiReconnect(BoardSettings::wifiSsid(),
                                BoardSettings::wifiPass(), 20000);
        Serial.printf("  -> %s  ip=%s\n", ok ? "OK" : "FAIL",
                      WiFi.localIP().toString().c_str());
        return;
    }
    if (line.startsWith("wifi set ")) {
        String rest = line.substring(strlen("wifi set "));
        rest.trim();
        int sp = rest.indexOf(' ');
        if (sp < 1 || sp >= (int)rest.length() - 1) {
            Serial.println(F("usage: wifi set <ssid> <pass>"));
            return;
        }
        String ssid = rest.substring(0, sp);
        String pass = rest.substring(sp + 1);
        BoardSettings::setWifi(ssid, pass);
        Serial.printf("saved ssid=\"%s\".  reconnecting...\n", ssid.c_str());
        bool ok = wifiReconnect(ssid, pass, 20000);
        Serial.printf("  -> %s  ip=%s\n", ok ? "OK" : "FAIL",
                      WiFi.localIP().toString().c_str());
        return;
    }
    Serial.printf("unknown: \"%s\"  (try `help`)\n", line.c_str());
}

void begin() {
    Serial.println(F("[cli] ready. type `help` for commands."));
}

void poll() {
    static String s_input;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            handle(s_input);
            s_input = String();
            continue;
        }
        if (s_input.length() < 200) s_input += c;
    }
}

} // namespace SerialCli
