// Declarations for routes_battery.cpp. Owns /api/batt/* + /api/smbus/*
// endpoints plus the clone-lab state machinery (brute force, MAC response,
// SMBus logger) that was previously intertwined in WebServer::start().
#pragma once
#include <ESPAsyncWebServer.h>
namespace RoutesBattery {
void registerRoutesBattery(AsyncWebServer *server);
}
