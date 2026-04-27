// AUTO-REFACTORED from web_server.cpp 2026-04-27. USB descriptor mode +
// soft-reboot endpoints — /api/usb/{mode (GET/POST), reboot}.
#pragma once
#include <ESPAsyncWebServer.h>
namespace RoutesUsb {
void registerRoutesUsb(AsyncWebServer *server);
}
