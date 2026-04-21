// AUTO-REFACTORED from web_server.cpp 2026-04-21. Plate self-update (OTA)
// endpoints — /api/ota/{info,check,pull,abort} plus multipart upload at
// /api/ota itself.
#pragma once
#include <ESPAsyncWebServer.h>
namespace RoutesOta {
void registerRoutesOta(AsyncWebServer *server);
}
