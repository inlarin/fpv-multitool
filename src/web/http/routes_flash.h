// Declarations for routes_flash.cpp — see that file for the endpoint
// implementations. Two entry points:
//   - registerRoutesFlash(server)  — register /api/flash/*, /api/elrs/*,
//     /api/otadata/*, /api/bridge/listen, /api/crsf/reboot_to_bl.
//   - executeFlash()               — called from the main loop when
//     WebState::flashState.flash_request is set. Runs on a dedicated xTask
//     internally; returns immediately after queuing.
#pragma once
#include <ESPAsyncWebServer.h>

namespace RoutesFlash {
void registerRoutesFlash(AsyncWebServer *server);
void executeFlash();
// Periodic tick from the web loop — handles sticky DFU session idle-timeout
// auto-close. Cheap (single millis() compare) — safe to call every loop tick.
void tick();
}
