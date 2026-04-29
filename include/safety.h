#pragma once

// Remote-deployment safety net for the ESP32 boards in this project.
//
// Three independent guarantees:
//
//   1. OTA rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is on in
//      arduino-esp32 v3 prebuilt bootloader). A new image is in
//      ESP_OTA_IMG_PENDING_VERIFY state for its first boot. If the
//      app does NOT explicitly call esp_ota_mark_app_valid_cancel_-
//      rollback() before any reboot, the bootloader marks the slot
//      ABORTED on the next attempt and rolls back to the previously-
//      valid app. We bind that confirmation to a deferred check
//      (`tickValidation`) -- only after N seconds of stable runtime
//      do we mark valid.
//
//   2. Boot-loop guard. An NVS-backed counter increments on every
//      boot at the VERY START of setup(). After tickValidation marks
//      the run good, the counter resets to 0. If the counter reaches
//      a threshold (5 by default) before any successful boot, we
//      enter "safe mode" -- the app skips all subsystem inits except
//      WiFi + WebServer, so OTA stays reachable for recovery.
//
//   3. Health endpoint. Exposed via WebServer (registered separately
//      under /api/health) so an external monitor can poll vitals
//      (uptime, ota state, boot count, free heap, wifi). Required for
//      remote deployments where there is no operator to look at the
//      LCD.
//
// These functions are deliberately board-agnostic so the Waveshare
// main app can adopt the same pattern in a follow-up commit.

#include <stdint.h>

namespace Safety {

// Call as the very first thing in setup() (after Serial.begin). Reads
// the boot counter, increments it, decides whether to enter safe mode,
// and reads the OTA partition state for later reporting.
void earlyBootCheck();

// True if the boot-loop counter triggered safe mode this boot. Caller
// (main) should skip everything except WiFi + WebServer in that case.
bool isSafeMode();

// Call from loop() every iteration. After the configured uptime
// threshold without a panic, marks the OTA app valid (cancels
// rollback) AND resets the boot counter. Cheap: noop after the first
// successful mark.
void tickValidation(uint32_t threshold_ms = 30000);

// Force-mark the OTA app valid right now. Use sparingly -- normally
// you let tickValidation do it once the runtime soak finishes. Useful
// from a settings UI as a manual override.
void markValidNow();

// Network watchdog. Call from loop() -- if WiFi.status() never reaches
// WL_CONNECTED within `timeout_ms` of boot, soft-reboot. Keeps a remote
// board from sitting forever in AP fallback if its STA target is
// temporarily down. Does NOTHING after the first successful
// WL_CONNECTED -- subsequent disconnects are NOT acted on (prevents
// reboot loops in flaky-network environments; the kernel's WiFi reconnect
// loop handles those).
//
// Defaults: 5 minutes. Pass `0` to disable.
void tickNetworkWatchdog(uint32_t timeout_ms = 5 * 60 * 1000);

// Health beacon. Call from loop() with the configured URL + interval
// (typically loaded from BoardSettings or a Waveshare-side equivalent).
// If `url` is empty or `interval_ms` is 0, this is a noop. Otherwise
// when the time-since-last-attempt exceeds `interval_ms`, POST a small
// JSON payload of vitals to `url` (5 s timeout, sync, no retry --
// retries happen on the next interval).
void tickBeacon(const char *url, uint32_t interval_ms);

// Force-send a beacon NOW regardless of cadence. Returns the HTTP
// status code (or negative for transport failure). Used by CLI / web
// "beacon now" actions.
int beaconSendNow(const char *url);

// Read-only accessors for /api/health and similar endpoints.
const char* otaStateStr();        // "VALID", "PENDING_VERIFY", etc.
uint32_t    bootCount();          // current value (counter not yet reset)
bool        wasValidatedThisBoot();
const char* lastResetReasonStr(); // "POWERON", "PANIC", "TASK_WDT", etc.

// Threshold for safe-mode entry. Compile-time constant; raise if your
// initial bring-up legitimately needs more cold-boot retries.
constexpr uint32_t SAFE_MODE_BOOT_THRESHOLD = 5;

} // namespace Safety
