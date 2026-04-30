#pragma once
#include "pin_config.h"

// Display singleton wrapper.
//
// On the Waveshare board this is the Arduino_GFX (SPI ST7789) driver
// for the onboard 172x320 LCD. On the SC01 Plus, the LCD is driven by
// LovyanGFX via BoardDisplay (different bus / different library), and
// the legacy local-UI screens (menu / *_tester / *_ui) are replaced by
// LVGL screens that talk to BoardDisplay directly. The Display:: API
// remains exposed on both boards as a stub returning nullptr from gfx()
// so legacy callers compile cleanly during the migration.

#if defined(BOARD_WT32_SC01_PLUS)
class Arduino_GFX;   // forward decl -- not actually instantiable on this board
#else
#include <Arduino_GFX_Library.h>
#endif

namespace Display {

void init();
Arduino_GFX* gfx();
void backlight(bool on);

} // namespace Display
