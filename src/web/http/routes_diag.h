// AUTO-REFACTORED from web_server.cpp 2026-04-27. Diagnostic endpoints —
// /api/cp2112/{log,info}, /api/i2c/{preflight,scan}. Small read-only
// inspectors that help triage bridge / I2C wiring issues without
// committing to a full bus session.
#pragma once
#include <ESPAsyncWebServer.h>
namespace RoutesDiag {
void registerRoutesDiag(AsyncWebServer *server);
}
