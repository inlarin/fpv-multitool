#include "usb_mode.h"
#include "pin_config.h"
#include <Preferences.h>
#include "pin_port.h"
#include <HardwareSerial.h>
#include "USB.h"
#include "USBCDC.h"

// USBSerial is only declared by Arduino when ARDUINO_USB_CDC_ON_BOOT=1. We set
// it to 0 to defer CDC registration — our own CDC instance registers from
// applyAtBoot() based on NVS mode.
static USBCDC g_cdc(0);

namespace {
    const char *KEY_NS   = "usb";
    const char *KEY_MODE = "mode";

    bool modeHasCDC(UsbDescriptorMode m) { return m == USB_MODE_CDC || m == USB_MODE_USB2TTL; }
    bool modeHasHID(UsbDescriptorMode m) { return m == USB_MODE_USB2I2C; }

    UsbDescriptorMode s_activeMode = USB_MODE_CDC;  // what applyAtBoot() actually set up
}

UsbDescriptorMode UsbMode::active() { return s_activeMode; }

UsbDescriptorMode UsbMode::load() {
    Preferences p;
    p.begin(KEY_NS, true);
    UsbDescriptorMode m = (UsbDescriptorMode)p.getUChar(KEY_MODE, USB_MODE_CDC);
    if (m > USB_MODE_USB2I2C) m = USB_MODE_CDC;    // guard against stale values
    p.end();
    return m;
}

void UsbMode::save(UsbDescriptorMode m) {
    Preferences p;
    p.begin(KEY_NS, false);
    p.putUChar(KEY_MODE, (uint8_t)m);
    p.end();
}

void UsbMode::switchAndReboot(UsbDescriptorMode m) {
    save(m);
    delay(200);
    ESP.restart();
}

const char *UsbMode::name(UsbDescriptorMode m) {
    switch (m) {
    case USB_MODE_CDC:     return "CDC (Serial)";
    case USB_MODE_USB2TTL: return "USB2TTL (UART bridge)";
    case USB_MODE_USB2I2C: return "USB2I2C (CP2112 HID)";
    default:               return "?";
    }
}

extern void CP2112_attach();

void UsbMode::applyAtBoot() {
    UsbDescriptorMode m = load();
    s_activeMode = m;
    Serial.printf("[UsbMode] boot mode: %s (%u)\n", name(m), (unsigned)m);

    if (modeHasCDC(m)) g_cdc.begin();
    if (modeHasHID(m)) CP2112_attach();

    USB.begin();
}

// ===== USB2TTL pump =====
// Pumps bytes transparently between the USB CDC interface and UART1 on
// ELRS_TX/ELRS_RX (Port B, GPIO 11/10). Called from main loop; no-op unless in
// USB2TTL mode. Serial1 is configured lazily on first call so we don't
// collide with other features (ELRS flasher, CRSF) that also use it.
void UsbMode::pumpLoop() {
    static bool inited = false;
    static uint32_t cur_baud = 0;
    if (load() != USB_MODE_USB2TTL) {
        if (inited) { PinPort::release(PinPort::PORT_B); }
        inited = false;
        return;
    }

    // The host sets its desired baud through the CDC line-coding request;
    // USBCDC::baudRate() returns what was negotiated.
    uint32_t want = g_cdc.baudRate();
    if (want == 0) want = 115200;
    if (!inited || want != cur_baud) {
        if (!inited) {
            if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "usb2ttl")) {
                // Port busy with another UART consumer or in different mode —
                // cannot pump. Retry on next loop iteration.
                return;
            }
        }
        Serial1.end();
        Serial1.begin(want, SERIAL_8N1,
                      PinPort::rx_pin(PinPort::PORT_B),
                      PinPort::tx_pin(PinPort::PORT_B));
        cur_baud = want;
        inited = true;
    }

    // Forward USB → UART
    while (g_cdc.available() && Serial1.availableForWrite()) {
        Serial1.write(g_cdc.read());
    }
    // Forward UART → USB
    while (Serial1.available() && g_cdc.availableForWrite()) {
        g_cdc.write(Serial1.read());
    }
}
