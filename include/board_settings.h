#pragma once

#include <stdint.h>
#include <Arduino.h>   // for String

// Persistent board-level settings, NVS-backed.
//
// Currently only stores the user-facing display rotation (0..3). Will
// grow over time to hold things like backlight level, sleep timeout,
// last-used catalog filter, etc -- so add new keys here, never sprinkle
// raw Preferences calls around the codebase.
//
// NVS namespace: "boardcfg" (max 15 chars, never rename without a
// migration -- old values become orphaned).
namespace BoardSettings {

void    begin();                            // open Preferences, called once from setup()
uint8_t rotation();                         // 0..3, default 0
void    setRotation(uint8_t rot);           // persists immediately

// Touch calibration: 8x uint16 produced by lgfx::LGFX_Device::calibrateTouch().
// Returns true if NVS held a previously-saved set; false if first boot.
bool    getTouchCalibrate(uint16_t out[8]);
void    setTouchCalibrate(const uint16_t in[8]);

// Wipe touch calibration (forces re-calibration on next boot).
void    clearTouchCalibrate();

// WiFi STA credentials (planned to be settable from a Settings UI later;
// for bring-up they're written once with hardcoded defaults). Returns
// "" if the key is unset.
String  wifiSsid();
String  wifiPass();
void    setWifi(const String &ssid, const String &pass);   // both at once
bool    hasWifiCreds();   // true if both ssid and pass are non-empty

// Health beacon: periodic outbound POST with device vitals to a
// monitoring endpoint. Required when the device is behind NAT/firewall
// and we can't reach it inbound. URL "" or interval 0 -> disabled.
String   beaconUrl();
uint32_t beaconIntervalMs();             // default 0 (disabled)
void     setBeacon(const String &url, uint32_t interval_ms);

} // namespace BoardSettings
