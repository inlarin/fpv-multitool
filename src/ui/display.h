#pragma once
#include <Arduino_GFX_Library.h>
#include "pin_config.h"

// Display singleton wrapper
namespace Display {

void init();
Arduino_GFX* gfx();
void backlight(bool on);

} // namespace Display
