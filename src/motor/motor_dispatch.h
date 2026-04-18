#pragma once
#include <Arduino.h>

// Shared web-initiated motor + servo sweep dispatcher. Previously duplicated
// in main.cpp::webBackgroundTask and wifi_app.cpp::runWifiApp (and drifting).
//
// Call once per outer loop iteration from whichever execution context is
// currently running the web server. Pass inMotorApp=true only from the
// on-device motor tester so that arm-from-web requests are suppressed to
// avoid fighting the local BOOT-button UI for DShot control.
namespace MotorDispatch {
    void pump(bool inMotorApp = false);
}
