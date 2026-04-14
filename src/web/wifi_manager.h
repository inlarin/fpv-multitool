#pragma once
#include <Arduino.h>

namespace WifiManager {

enum Mode { MODE_OFF, MODE_AP, MODE_STA };

bool startAP(const char* ssid = "FPV-MultiTool", const char* pass = "fpv12345");
bool startSTA(const char* ssid, const char* pass, uint32_t timeoutMs = 15000);
void stop();

Mode currentMode();
String getIP();
String getSSID();
int getRSSI();
int clientCount();

// Persistent credentials (NVS)
bool saveCredentials(const char* ssid, const char* pass);
bool loadCredentials(String &ssid, String &pass);
void clearCredentials();

} // namespace WifiManager
