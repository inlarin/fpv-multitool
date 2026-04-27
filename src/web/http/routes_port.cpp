// AUTO-REFACTORED from web_server.cpp 2026-04-27. See routes_port.h.
#include "routes_port.h"

#include "../../core/pin_port.h"
#include "../../core/usb_mode.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Wire.h>

namespace RoutesPort {

void registerRoutesPort(AsyncWebServer *s_server) {

    // Port B mode selector ------------------------------------------------
    s_server->on("/api/port/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument j;
        auto d = PinPort::def(PinPort::PORT_B);
        j["name"]       = d ? d->name : "B";
        j["pin_a"]      = d ? d->pin_a : -1;
        j["pin_b"]      = d ? d->pin_b : -1;
        PortMode cur    = PinPort::currentMode(PinPort::PORT_B);
        const char *ow  = PinPort::currentOwner(PinPort::PORT_B);
        j["mode"]       = (int)cur;
        j["mode_name"]  = PinPort::modeName(cur);
        j["owner"]      = ow ? ow : "";
        PortMode pref   = PinPort::preferredMode(PinPort::PORT_B);
        j["preferred"]  = (int)pref;
        j["preferred_name"] = PinPort::modeName(pref);
        JsonArray arr   = j["modes"].to<JsonArray>();
        for (int i = 0; i < (int)PORT_MODE_COUNT; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["id"]   = i;
            o["name"] = PinPort::modeName((PortMode)i);
        }
        String out; serializeJson(j, out);
        req->send(200, "application/json", out);
    });

    s_server->on("/api/port/preferred", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("mode", true)) {
            req->send(400, "text/plain", "Missing 'mode' form param");
            return;
        }
        int v = req->getParam("mode", true)->value().toInt();
        if (v < 0 || v >= (int)PORT_MODE_COUNT) {
            req->send(400, "text/plain", "Invalid mode id");
            return;
        }
        PinPort::setPreferredMode(PinPort::PORT_B, (PortMode)v);
        // Also try to apply immediately: release current, re-acquire.
        // If a feature is actively using the port in a different mode,
        // release() will tear it down; the feature will notice on next loop.
        PinPort::release(PinPort::PORT_B);
        if ((PortMode)v != PORT_IDLE) {
            PinPort::acquire(PinPort::PORT_B, (PortMode)v, "boot");
        }
        req->send(200, "text/plain", "Saved");
    });

    s_server->on("/api/port/release", HTTP_POST, [](AsyncWebServerRequest *req) {
        PinPort::release(PinPort::PORT_B);
        req->send(200, "text/plain", "Released");
    });

    // Passive listener diagnostic — open UART at a requested baud, count
    // every byte that arrives in N ms, return a hex preview. Doesn't send
    // anything — just confirms whether the wire carries a signal at all.
    // Useful when autodetect says "no signal" and we want to distinguish
    // "nothing ever arrived" (wiring/power issue) from "arrived but didn't
    // look like what we expected".
    s_server->on("/api/port/probe_rx", HTTP_POST, [](AsyncWebServerRequest *req) {
        uint32_t baud = req->hasParam("baud", true)
            ? strtoul(req->getParam("baud", true)->value().c_str(), nullptr, 0) : 115200;
        uint32_t ms = req->hasParam("ms", true)
            ? strtoul(req->getParam("ms", true)->value().c_str(), nullptr, 0) : 1000;
        if (ms > 5000) ms = 5000;
        bool swap = req->hasParam("swap", true)
            ? req->getParam("swap", true)->value() == "1"
            : PinPort::swapped(PinPort::PORT_B);

        PinPort::setSwapped(PinPort::PORT_B, swap);
        PinPort::release(PinPort::PORT_B);
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "probe_rx")) {
            req->send(409, "text/plain", "Port B busy");
            return;
        }
        int rxPin = PinPort::rx_pin(PinPort::PORT_B);
        int txPin = PinPort::tx_pin(PinPort::PORT_B);
        Serial1.end();
        Serial1.begin(baud, SERIAL_8N1, rxPin, txPin);

        static uint8_t buf[512];
        size_t got = 0;
        uint32_t start = millis();
        while (millis() - start < ms && got < sizeof(buf)) {
            if (Serial1.available()) buf[got++] = (uint8_t)Serial1.read();
            else delay(1);
        }

        JsonDocument d;
        d["baud"]   = baud;
        d["ms"]     = ms;
        d["swap"]   = swap;
        d["rx_pin"] = rxPin;
        d["tx_pin"] = txPin;
        d["bytes"]  = got;
        // Preview first 64 bytes as hex
        String hex; hex.reserve(132);
        for (size_t i = 0; i < got && i < 64; i++) {
            char b[4]; snprintf(b, sizeof(b), "%02x", buf[i]); hex += b;
        }
        d["hex_preview"] = hex;
        // Count non-idle bytes (anything other than 0x00 or 0xFF)
        int interesting = 0;
        for (size_t i = 0; i < got; i++) if (buf[i] != 0x00 && buf[i] != 0xFF) interesting++;
        d["non_idle_count"] = interesting;

        PinPort::release(PinPort::PORT_B);
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    // Manual swap override (advanced) — toggles PinPort pin_a/pin_b
    // assignment for I2C/UART and re-acquires if port is live.
    s_server->on("/api/port/swap", HTTP_POST, [](AsyncWebServerRequest *req) {
        bool swap = !PinPort::swapped(PinPort::PORT_B);
        if (req->hasParam("swap", true)) {
            swap = req->getParam("swap", true)->value() == "1";
        }
        PinPort::setSwapped(PinPort::PORT_B, swap);
        JsonDocument d;
        d["swapped"] = PinPort::swapped(PinPort::PORT_B);
        d["tx_pin"]  = PinPort::tx_pin(PinPort::PORT_B);
        d["rx_pin"]  = PinPort::rx_pin(PinPort::PORT_B);
        d["sda_pin"] = PinPort::sda_pin(PinPort::PORT_B);
        d["scl_pin"] = PinPort::scl_pin(PinPort::PORT_B);
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    // Auto-detect wiring — tries both pin_a↔pin_b assignments and reports
    // which one produced a valid signal. Saves swap flag to NVS on success.
    //   signal=crsf   → listen for CRSF frame start 0xC8 @ 420000 8N1
    //   signal=sbus   → listen for SBUS start 0x0F @ 100000 8E2 inverted
    //   signal=ibus   → listen for iBus header 0x20,0x40 @ 115200 8N1
    //   signal=i2c    → I2C bus scan 0x08..0x77, success if ≥1 ACK
    //   signal=elrs_rom → ROM bootloader SYNC @ 115200 (user must hold BOOT)
    // Returns JSON with "swap_used", "detected", and the swap flag state.
    s_server->on("/api/port/autodetect", HTTP_POST, [](AsyncWebServerRequest *req) {
        String signal = req->hasParam("signal", true)
            ? req->getParam("signal", true)->value() : "crsf";

        auto tryOnce = [&](bool swap, unsigned long waitMs) -> bool {
            PinPort::setSwapped(PinPort::PORT_B, swap);
            PinPort::release(PinPort::PORT_B);

            if (signal == "i2c") {
                if (!PinPort::acquire(PinPort::PORT_B, PORT_I2C, "autodetect")) return false;
                Wire1.end();
                int sda = PinPort::sda_pin(PinPort::PORT_B);
                int scl = PinPort::scl_pin(PinPort::PORT_B);
                pinMode(sda, INPUT_PULLUP);
                pinMode(scl, INPUT_PULLUP);
                Wire1.begin(sda, scl);
                Wire1.setClock(100000);
                Wire1.setTimeOut(30);
                for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
                    Wire1.beginTransmission(addr);
                    if (Wire1.endTransmission() == 0) return true;
                }
                return false;
            }

            // UART-based detection — pick baud/framing per signal.
            if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "autodetect")) return false;
            Serial1.end();
            int rxPin = PinPort::rx_pin(PinPort::PORT_B);
            int txPin = PinPort::tx_pin(PinPort::PORT_B);

            uint32_t baud; uint32_t cfg; bool invert;
            uint8_t syncByte1 = 0, syncByte2 = 0; bool needTwo = false;
            if (signal == "crsf") {
                baud = 420000; cfg = SERIAL_8N1; invert = false;
                syncByte1 = 0xC8;  // CRSF broadcast address
            } else if (signal == "sbus") {
                baud = 100000; cfg = SERIAL_8E2; invert = true;
                syncByte1 = 0x0F;
            } else if (signal == "ibus") {
                baud = 115200; cfg = SERIAL_8N1; invert = false;
                syncByte1 = 0x20; syncByte2 = 0x40; needTwo = true;
            } else if (signal == "elrs_rom") {
                // ROM bootloader doesn't emit unsolicited bytes. We send a SYNC
                // and wait for any response byte. A SYNC is 36 bytes with SLIP
                // framing; we'll crudely emit 0xC0 then flood 0x07 0x07 0x12 0x20
                // followed by 32× 0x55, then listen.
                baud = 115200; cfg = SERIAL_8N1; invert = false;
                syncByte1 = 0x00; // any byte counts
            } else {
                baud = 420000; cfg = SERIAL_8N1; invert = false; syncByte1 = 0xC8;
            }

            Serial1.begin(baud, cfg, rxPin, txPin, invert);

            if (signal == "elrs_rom") {
                // emit SYNC framing
                static const uint8_t syncPayload[] = {0x07, 0x07, 0x12, 0x20};
                Serial1.write((uint8_t)0xC0);
                Serial1.write((uint8_t)0x00);       // direction
                Serial1.write((uint8_t)0x08);       // SYNC cmd
                Serial1.write((uint8_t)36); Serial1.write((uint8_t)0);  // size LE
                Serial1.write((uint8_t)0); Serial1.write((uint8_t)0);   // cksum x4 (ignored by ROM for SYNC)
                Serial1.write((uint8_t)0); Serial1.write((uint8_t)0);
                Serial1.write(syncPayload, sizeof(syncPayload));
                for (int i = 0; i < 32; i++) Serial1.write((uint8_t)0x55);
                Serial1.write((uint8_t)0xC0);
                Serial1.flush();
            }

            uint32_t start = millis();
            bool sawFirst = false;
            while (millis() - start < waitMs) {
                if (!Serial1.available()) { delay(2); continue; }
                uint8_t b = (uint8_t)Serial1.read();
                if (signal == "elrs_rom") {
                    // Any non-0xC0-only byte = bootloader replied
                    if (b != 0xC0) return true;
                }
                if (!needTwo) {
                    if (b == syncByte1) return true;
                } else {
                    if (!sawFirst) { sawFirst = (b == syncByte1); }
                    else if (b == syncByte2) return true;
                    else sawFirst = (b == syncByte1);
                }
            }
            return false;
        };

        bool origSwap = PinPort::swapped(PinPort::PORT_B);
        unsigned long perSideMs = 1500;

        bool okDirect = tryOnce(false, perSideMs);
        bool okSwapped = false;
        if (!okDirect) {
            okSwapped = tryOnce(true, perSideMs);
        }

        // Leave the working swap (or revert to original if neither worked).
        bool detected = okDirect || okSwapped;
        bool finalSwap = okDirect ? false : (okSwapped ? true : origSwap);
        PinPort::setSwapped(PinPort::PORT_B, finalSwap);
        PinPort::release(PinPort::PORT_B);

        JsonDocument d;
        d["signal"]      = signal;
        d["detected"]    = detected;
        d["swap_used"]   = detected ? finalSwap : (bool)false;
        d["swapped_now"] = PinPort::swapped(PinPort::PORT_B);
        d["tx_pin"]      = PinPort::tx_pin(PinPort::PORT_B);
        d["rx_pin"]      = PinPort::rx_pin(PinPort::PORT_B);
        d["sda_pin"]     = PinPort::sda_pin(PinPort::PORT_B);
        d["scl_pin"]     = PinPort::scl_pin(PinPort::PORT_B);
        d["note"]        = detected
            ? (finalSwap ? "signal found with pins SWAPPED — flag persisted to NVS"
                         : "signal found with DIRECT pinout (no swap needed)")
            : "no signal detected in either pinout — check GND/power, or signal type";
        String out; serializeJson(d, out);
        req->send(200, "application/json", out);
    });

    // ===== Setup wizard — HOST / BRIDGE + device preset =====
    // One-shot configurator that derives both (USB descriptor) and
    // (Port B electrical mode) from the user's intent:
    //   device  ∈ {battery, receiver, motor, advanced}
    //   role    ∈ {host, bridge}
    // HOST   = the board itself talks to the device (CDC-only USB).
    // BRIDGE = PC talks to the device via the board as transparent proxy.
    //   - battery  → USB2I2C (CP2112 HID)
    //   - receiver → USB2TTL (CDC + UART pump)
    //   - advanced → USB2TTL
    //   - motor    → not supported (no PC-side toolchain for servo/DShot);
    //                UI doesn't expose this combo.

    auto setupStringsToPreset = [](const String &device, const String &role,
                                   UsbDescriptorMode &outUsb, PortMode &outPort,
                                   String &outErr) -> bool {
        bool bridge = (role == "bridge");
        if (device == "battery") {
            outUsb  = bridge ? USB_MODE_USB2I2C : USB_MODE_CDC;
            outPort = PORT_I2C;
        } else if (device == "receiver") {
            outUsb  = bridge ? USB_MODE_USB2TTL : USB_MODE_CDC;
            outPort = PORT_UART;
        } else if (device == "motor") {
            if (bridge) { outErr = "motor has no bridge mode"; return false; }
            outUsb  = USB_MODE_CDC;
            outPort = PORT_PWM;
        } else if (device == "advanced") {
            outUsb  = bridge ? USB_MODE_USB2TTL : USB_MODE_CDC;
            outPort = bridge ? PORT_UART : PORT_GPIO;
        } else if (device == "idle") {
            outUsb  = USB_MODE_CDC;
            outPort = PORT_IDLE;
        } else {
            outErr = "unknown device";
            return false;
        }
        return true;
    };

    auto presetToStrings = [](UsbDescriptorMode usb, PortMode port,
                              String &outDevice, String &outRole) {
        // Infer (device, role) from the raw pair. USB being CDC implies HOST.
        if (usb == USB_MODE_USB2I2C) {
            outDevice = "battery";  outRole = "bridge"; return;
        }
        if (usb == USB_MODE_USB2TTL) {
            // USB2TTL is shared by Receiver BRIDGE and Advanced BRIDGE; the
            // distinction is only UX sugar — we surface Receiver by default.
            outDevice = "receiver"; outRole = "bridge"; return;
        }
        // CDC — role is always HOST; device is derived from Port B.
        outRole = "host";
        switch (port) {
            case PORT_I2C:  outDevice = "battery";  break;
            case PORT_UART: outDevice = "receiver"; break;
            case PORT_PWM:  outDevice = "motor";    break;
            case PORT_GPIO: outDevice = "advanced"; break;
            case PORT_IDLE: outDevice = "idle";     break;
            default:        outDevice = "unknown";  break;
        }
    };

    s_server->on("/api/setup/status", HTTP_GET,
        [presetToStrings](AsyncWebServerRequest *req) {
            UsbDescriptorMode activeUsb = UsbMode::active();
            UsbDescriptorMode prefUsb   = UsbMode::load();
            PortMode activePort = PinPort::currentMode(PinPort::PORT_B);
            PortMode prefPort   = PinPort::preferredMode(PinPort::PORT_B);

            String activeDev, activeRole, prefDev, prefRole;
            presetToStrings(activeUsb, activePort, activeDev, activeRole);
            presetToStrings(prefUsb, prefPort, prefDev, prefRole);

            JsonDocument d;
            JsonObject a = d["active"].to<JsonObject>();
            a["device"]    = activeDev;
            a["role"]      = activeRole;
            a["usb"]       = UsbMode::name(activeUsb);
            a["port"]      = PinPort::modeName(activePort);
            a["owner"]     = PinPort::currentOwner(PinPort::PORT_B);

            JsonObject p = d["preferred"].to<JsonObject>();
            p["device"]    = prefDev;
            p["role"]      = prefRole;
            p["usb"]       = UsbMode::name(prefUsb);
            p["port"]      = PinPort::modeName(prefPort);

            d["reboot_pending"] = (activeUsb != prefUsb);

            String out; serializeJson(d, out);
            req->send(200, "application/json", out);
        });

    s_server->on("/api/setup/apply", HTTP_POST,
        [setupStringsToPreset](AsyncWebServerRequest *req) {
            if (!req->hasParam("device", true) || !req->hasParam("role", true)) {
                req->send(400, "text/plain", "Need form params 'device' and 'role'");
                return;
            }
            String device = req->getParam("device", true)->value();
            String role   = req->getParam("role", true)->value();
            UsbDescriptorMode usb;
            PortMode port;
            String err;
            if (!setupStringsToPreset(device, role, usb, port, err)) {
                req->send(400, "text/plain", err);
                return;
            }

            UsbDescriptorMode prevUsb = UsbMode::load();
            bool usbChanged = (usb != prevUsb);

            UsbMode::save(usb);
            PinPort::setPreferredMode(PinPort::PORT_B, port);

            // Apply Port B immediately (no reboot needed for Port B changes).
            PinPort::release(PinPort::PORT_B);
            if (port != PORT_IDLE) {
                PinPort::acquire(PinPort::PORT_B, port, "setup");
            }

            JsonDocument d;
            d["ok"]             = true;
            d["device"]         = device;
            d["role"]           = role;
            d["usb"]            = UsbMode::name(usb);
            d["port"]           = PinPort::modeName(port);
            d["reboot_needed"]  = usbChanged;
            d["reboot_reason"]  = usbChanged
                ? "USB descriptor fixed at boot — reboot to re-enumerate"
                : "";
            String out; serializeJson(d, out);
            req->send(200, "application/json", out);
        });
}

} // namespace RoutesPort
