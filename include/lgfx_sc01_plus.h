#pragma once

// LovyanGFX driver config for the Wireless-Tag WT32-SC01 Plus.
// Panel: ST7796 320x480 portrait, 8-bit 8080 parallel bus.
// Backlight: GPIO 45, active-HIGH.
//
// Pin map mirrored from include/pin_config_sc01_plus.h, which itself was
// pulled from the openHASP user_setups/esp32s3/wt32-sc01-plus.ini reference
// (verified against homeding + the factory dump's 8080_lcd_esp32s3.c strings).
//
// IMPORTANT: this header pulls in the full LovyanGFX template machinery.
// It expects -DLGFX_USE_V1 from build_flags (set in platformio.ini's
// [env:wt32_sc01_plus_lcd]). Do NOT include from the main esp32s3 env —
// it's WT32-SC01-Plus specific.

#include <LovyanGFX.hpp>

#include "pin_config_sc01_plus.h"

class LGFX_SC01Plus : public lgfx::LGFX_Device {
    lgfx::Panel_ST7796   _panel;
    lgfx::Bus_Parallel8  _bus;
    lgfx::Light_PWM      _light;

public:
    LGFX_SC01Plus() {
        // ----- Bus: 8-bit 8080 parallel via the ESP32-S3 i80 LCD controller -----
        {
            auto cfg = _bus.config();
            cfg.freq_write = 20000000;        // 20 MHz PCLK -- ST7796 max ~33 MHz, 20 is safe and fast
            cfg.pin_wr     = SC01P_LCD_WR;    // 47
            cfg.pin_rd     = SC01P_LCD_RD;    // -1 (reads not wired)
            cfg.pin_rs     = SC01P_LCD_DC;    // 0 (= BOOT button — never wire anything else here)
            cfg.pin_d0     = SC01P_LCD_D0;    // 9
            cfg.pin_d1     = SC01P_LCD_D1;    // 46
            cfg.pin_d2     = SC01P_LCD_D2;    // 3
            cfg.pin_d3     = SC01P_LCD_D3;    // 8
            cfg.pin_d4     = SC01P_LCD_D4;    // 18
            cfg.pin_d5     = SC01P_LCD_D5;    // 17
            cfg.pin_d6     = SC01P_LCD_D6;    // 16
            cfg.pin_d7     = SC01P_LCD_D7;    // 15
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }

        // ----- Panel: ST7796 native 320x480 -----
        {
            auto cfg = _panel.config();
            cfg.pin_cs           = SC01P_LCD_CS;   // -1 (tied low on PCB)
            cfg.pin_rst          = SC01P_LCD_RST;  // 4 (shared with FT6336 RST -- driven HIGH at init)
            cfg.pin_busy         = -1;
            cfg.panel_width      = SC01P_LCD_WIDTH;   // 320
            cfg.panel_height     = SC01P_LCD_HEIGHT;  // 480
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 2;       // panel is mounted 180° rotated on the SC01 Plus PCB —
                                            // without this, setRotation(0) shows the image upside-down.
                                            // Verified on real board 2026-04-29 (FT6336 unit).
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = false;   // pin_rd = -1, no readback
            cfg.invert           = true;    // ST7796 typical -- without this the colors are inverted
            cfg.rgb_order        = false;   // BGR (most ST7796 boards including SC01 Plus)
            cfg.dlen_16bit       = false;   // 8-bit data lane, not 16
            cfg.bus_shared       = false;
            _panel.config(cfg);
        }

        // ----- Backlight: PWM on GPIO 45, active-HIGH -----
        {
            auto cfg = _light.config();
            cfg.pin_bl      = SC01P_LCD_BL;   // 45
            cfg.invert      = false;          // HIGH = backlight on
            cfg.freq        = 12000;          // 12 kHz, above audible
            cfg.pwm_channel = 7;              // arbitrary unused LEDC channel
            _light.config(cfg);
            _panel.setLight(&_light);
        }

        setPanel(&_panel);
    }
};
