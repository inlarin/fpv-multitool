#pragma once
#include <Arduino.h>

// Boot-time USB descriptor selection. Saved in NVS, applied on next reboot.
// The descriptor is fixed at enumeration — we can't switch at runtime without
// forcing re-enumeration. So: set from NVS on boot, reboot to switch.

enum UsbDescriptorMode : uint8_t {
    USB_MODE_CDC     = 0,  // USB-CDC only — flashing, Serial debug (default)
    USB_MODE_USB2TTL = 1,  // USB-CDC + auto-bridge to hardware UART1 (transparent serial)
    USB_MODE_USB2I2C = 2,  // HID-only CP2112 emulator — SMBus/I2C tools (e.g. DJI Battery Killer)
};

namespace UsbMode {
    UsbDescriptorMode load();                     // preferred mode from NVS
    UsbDescriptorMode active();                   // mode actually applied at this boot
    void save(UsbDescriptorMode m);
    void applyAtBoot();                           // called from setup() before USB.begin()
    const char *name(UsbDescriptorMode m);
    void switchAndReboot(UsbDescriptorMode m);

    // USB2TTL pump: call from main loop. Configures Serial1 on first call,
    // then forwards bytes both directions between the CDC serial and UART1.
    // No-op when current mode is not USB2TTL.
    void pumpLoop();
}
