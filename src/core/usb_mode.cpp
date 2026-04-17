#include "usb_mode.h"
#include <Preferences.h>
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
}

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
    Serial.printf("[UsbMode] boot mode: %s (%u)\n", name(m), (unsigned)m);

    if (modeHasCDC(m)) g_cdc.begin();
    if (modeHasHID(m)) CP2112_attach();

    USB.begin();
}
