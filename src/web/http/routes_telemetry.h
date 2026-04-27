// AUTO-REFACTORED from web_server.cpp 2026-04-27. Read-only telemetry +
// signal-source endpoints that don't fit anywhere else:
//   /api/servo/state       — servo PWM + sweep recorder snapshot
//   /api/esc/telem/{start,stop,state} — KISS / BLHeli_32 ESC telemetry
//   /api/rc/{start,stop,state}        — SBUS / iBus / PPM sniffer
#pragma once
#include <ESPAsyncWebServer.h>
namespace RoutesTelemetry {
void registerRoutesTelemetry(AsyncWebServer *server);
}
