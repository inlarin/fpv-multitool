// AUTO-REFACTORED from web_server.cpp 2026-04-27. Port-B mode arbitrator
// + setup-wizard endpoints. Group covers /api/port/* (status, preferred,
// release, probe_rx, swap, autodetect) and /api/setup/{status, apply}.
// Setup wizard sits here because it derives Port B preset from the user
// choice and persists both in NVS — same domain as the port arbitrator.
#pragma once
#include <ESPAsyncWebServer.h>
namespace RoutesPort {
void registerRoutesPort(AsyncWebServer *server);
}
