#pragma once
#include <Arduino.h>

// Standalone servo PWM driver (used by web server and menu UI)
namespace ServoPWM {

bool start(uint8_t pin, int freq);
void stop();
void setFrequency(int hz);
void setPulse(int us);
bool isActive();

} // namespace ServoPWM
