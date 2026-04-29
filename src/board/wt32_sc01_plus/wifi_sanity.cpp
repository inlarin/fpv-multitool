// WT32-SC01 Plus -- Sprint 32 step 2d WiFi STA sanity sketch.
//
// Built ONLY by [env:wt32_sc01_plus_wifi]. Brings up the ESP32-S3 WiFi
// in STA mode using credentials from NVS (BoardSettings::wifiSsid/Pass).
// On the very first boot the NVS keys are empty -- we write hardcoded
// defaults exactly once so the device can join its target network.
//
// "Hardcoded defaults" is bring-up-only. The follow-up plan ("plan B"
// per user 2026-04-29) is Serial CLI provisioning, replacing the
// hardcoded block. After that, SSID/pass come from the user, not the
// source tree.
//
// What this sketch verifies:
//   - WiFi peripheral starts and joins the AP
//   - DHCP leases an IP, gateway is reachable (ping-equivalent: TCP
//     connect to the gateway port 80, or HTTP GET on a public host)
//   - Reboot picks up creds from NVS without re-writing them
//
// Output: USB CDC + on-screen via LovyanGFX.

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "board_settings.h"
#include "lgfx_sc01_plus.h"

static LGFX_SC01Plus lcd;

// One-shot defaults, only written if NVS has nothing yet.
static const char *DEFAULT_SSID = "SilentPlace";
static const char *DEFAULT_PASS = "f1r3Wall!";

static void lcdLine(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lcd.println(buf);
    Serial.println(buf);
}

static const char *statusToStr(wl_status_t s) {
    switch (s) {
        case WL_IDLE_STATUS:     return "IDLE";
        case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL";
        case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
        case WL_CONNECTED:       return "CONNECTED";
        case WL_CONNECT_FAILED:  return "CONNECT_FAILED";
        case WL_CONNECTION_LOST: return "CONNECTION_LOST";
        case WL_DISCONNECTED:    return "DISCONNECTED";
        default:                 return "?";
    }
}

static bool connectStation(const String &ssid, const String &pass, uint32_t timeout_ms) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true);   // clear stale config + erase saved creds (we manage them ourselves)
    delay(50);
    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t t0 = millis();
    wl_status_t st = WiFi.status();
    while (st != WL_CONNECTED && (millis() - t0) < timeout_ms) {
        delay(250);
        wl_status_t newst = WiFi.status();
        if (newst != st) {
            st = newst;
            lcdLine("  status: %s", statusToStr(st));
        }
        // Bail early on hard failures.
        if (st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED) break;
    }
    return WiFi.status() == WL_CONNECTED;
}

static void httpGetTest(const char *url) {
    lcdLine("HTTP GET %s", url);
    HTTPClient http;
    if (!http.begin(url)) {
        lcdLine("  begin FAILED");
        return;
    }
    int code = http.GET();
    int len  = (int)http.getSize();
    lcdLine("  code=%d  len=%d", code, len);
    http.end();
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { delay(10); }

    Serial.println();
    Serial.println(F("================ WT32-SC01 Plus WiFi sanity ================"));

    lcd.init();
    lcd.setRotation(0);
    lcd.setBrightness(255);
    lcd.fillScreen(TFT_BLACK);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setTextSize(2);
    lcd.setCursor(4, 4);

    lcdLine("WiFi STA sanity");

    BoardSettings::begin();

    // First boot: seed NVS with the hardcoded defaults. After that, the
    // creds live in NVS and survive reboot/reflash. (NVS is preserved
    // across `pio run -t upload` -- only `pio run -t erase` wipes it.)
    if (!BoardSettings::hasWifiCreds()) {
        lcdLine("NVS empty -- seeding");
        BoardSettings::setWifi(DEFAULT_SSID, DEFAULT_PASS);
    } else {
        lcdLine("NVS has creds -- using");
    }

    String ssid = BoardSettings::wifiSsid();
    String pass = BoardSettings::wifiPass();
    lcdLine("ssid: %s", ssid.c_str());
    lcdLine("pass: %s", pass.length() > 0 ? "(set)" : "(empty!)");
    lcdLine("");

    lcdLine("connecting...");
    bool ok = connectStation(ssid, pass, /* timeout_ms = */ 20000);

    if (!ok) {
        lcd.setTextColor(TFT_RED, TFT_BLACK);
        lcdLine("");
        lcdLine("CONNECT FAILED");
        lcdLine("status: %s", statusToStr(WiFi.status()));
        return;
    }

    IPAddress ip = WiFi.localIP();
    IPAddress gw = WiFi.gatewayIP();
    int rssi = WiFi.RSSI();
    lcdLine("");
    lcdLine("ip:   %s", ip.toString().c_str());
    lcdLine("gw:   %s", gw.toString().c_str());
    lcdLine("rssi: %d dBm", rssi);
    lcdLine("");

    // Internet reachability spot-check. http://example.com is small and
    // stable. Failure here means NAT/DNS/firewall, not WiFi peripheral.
    httpGetTest("http://example.com/");

    lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    lcdLine("");
    lcdLine("ALL OK");
}

// ---- Serial CLI -------------------------------------------------------------
//
// Tiny one-liner protocol so you can change creds without a reflash:
//
//   wifi set <ssid> <pass>   -- write to NVS and reconnect
//   wifi show                -- print current ssid (not pass)
//   wifi clear               -- wipe creds from NVS (reboot to re-seed)
//   wifi reconnect           -- bounce the link with current NVS creds
//   help                     -- print this list
//
// Whitespace splits at the first space after "set"; the rest of the line
// is treated as `<ssid> <pass>` with one space between. This is the
// minimal sufficient form for bring-up; the catalog UI will replace it
// with a proper Settings screen later.

static void cliPrintHelp() {
    Serial.println(F("commands:"));
    Serial.println(F("  wifi set <ssid> <pass>   write+reconnect"));
    Serial.println(F("  wifi show                print current ssid"));
    Serial.println(F("  wifi clear               wipe NVS creds"));
    Serial.println(F("  wifi reconnect           bounce link"));
    Serial.println(F("  help                     this list"));
}

static void cliHandle(String line) {
    line.trim();
    if (line.length() == 0) return;

    if (line.equalsIgnoreCase("help")) {
        cliPrintHelp();
        return;
    }
    if (line.equalsIgnoreCase("wifi show")) {
        Serial.printf("ssid=\"%s\"  pass_set=%s\n",
                      BoardSettings::wifiSsid().c_str(),
                      BoardSettings::wifiPass().length() ? "true" : "false");
        return;
    }
    if (line.equalsIgnoreCase("wifi clear")) {
        BoardSettings::setWifi(String(""), String(""));
        Serial.println("creds cleared (reboot to re-seed defaults)");
        return;
    }
    if (line.equalsIgnoreCase("wifi reconnect")) {
        Serial.println("reconnecting...");
        bool ok = connectStation(BoardSettings::wifiSsid(),
                                 BoardSettings::wifiPass(), 20000);
        Serial.printf("  -> %s  ip=%s\n",
                      ok ? "OK" : "FAIL",
                      WiFi.localIP().toString().c_str());
        return;
    }
    if (line.startsWith("wifi set ")) {
        // Form: "wifi set <ssid> <pass>". SSID = first word after "set",
        // password = everything after the space following SSID. This
        // way a password may contain spaces; an SSID may not (rare in
        // practice and not worth a quoting protocol on serial).
        String rest = line.substring(strlen("wifi set "));
        rest.trim();
        int sp = rest.indexOf(' ');
        if (sp < 1 || sp >= (int)rest.length() - 1) {
            Serial.println("usage: wifi set <ssid> <pass>");
            return;
        }
        String ssid = rest.substring(0, sp);
        String pass = rest.substring(sp + 1);
        BoardSettings::setWifi(ssid, pass);
        Serial.printf("saved ssid=\"%s\".  reconnecting...\n", ssid.c_str());
        bool ok = connectStation(ssid, pass, 20000);
        Serial.printf("  -> %s  ip=%s\n",
                      ok ? "OK" : "FAIL",
                      WiFi.localIP().toString().c_str());
        return;
    }

    Serial.printf("unknown: \"%s\"  (try `help`)\n", line.c_str());
}

void loop() {
    // Drain Serial line-by-line, dispatch to the CLI.
    static String s_input;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            cliHandle(s_input);
            s_input = String();
            continue;
        }
        if (s_input.length() < 200) s_input += c;
    }

    static uint32_t last = 0;
    if (millis() - last >= 5000) {
        last = millis();
        Serial.printf("alive  status=%s  rssi=%d  free heap=%u\n",
                      statusToStr(WiFi.status()), WiFi.RSSI(),
                      (unsigned)ESP.getFreeHeap());
    }
    delay(20);
}
