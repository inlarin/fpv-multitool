// AUTO-REFACTORED from web_server.cpp 2026-04-27. WiFi STA-credentials +
// async scan endpoints — /api/wifi/{save,clear,scan,scan_results}.
#pragma once
#include <ESPAsyncWebServer.h>
namespace RoutesWifi {
void registerRoutesWifi(AsyncWebServer *server);
}
