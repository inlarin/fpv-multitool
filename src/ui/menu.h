#pragma once
#include <Arduino.h>

// App identifiers
enum AppId {
    APP_NONE = -1,
    APP_USB2TTL = 0,
    APP_SERVO,
    APP_MOTOR,
    APP_BATTERY,
    APP_WIFI,
    APP_COUNT
};

namespace Menu {

void draw();                    // Draw/redraw the menu
AppId update(int btnEvent);     // Process button event, returns selected app or APP_NONE

} // namespace Menu
