// AUTO-REFACTORED from web_server.cpp 2026-04-27. CRSF telemetry service
// endpoints — /api/crsf/{start,stop,state,reboot}. Per-RX param queries
// were merged into /api/elrs/* during the Option A receiver-tab merge,
// so this file only covers the live link/telemetry service lifecycle.
#pragma once
#include <ESPAsyncWebServer.h>
namespace RoutesCrsf {
void registerRoutesCrsf(AsyncWebServer *server);
}
