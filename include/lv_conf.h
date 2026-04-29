// LVGL v9 config for the WT32-SC01 Plus.
//
// Picked up via -DLV_CONF_INCLUDE_SIMPLE in platformio.ini, which makes
// the library include <lv_conf.h> (not relative). Anything not set
// here falls back to lv_conf_internal.h defaults, which are sane for
// embedded use.
//
// Scope: only the WT32-SC01 Plus envs include this. The Waveshare
// board's main esp32s3 env is not LVGL-based and never reaches it.

#pragma once

// ---- Color ----
// 16-bit RGB565 -- matches LovyanGFX's default Bus_Parallel8 path.
#define LV_COLOR_DEPTH 16

// Stay on plain RGB565 for rendering -- this is LVGL's well-tested
// path, alpha-blending math is correct here. Byte swap to ST7796's
// big-endian wire format happens AFTER blending in flush_cb via
// lv_draw_sw_rgb565_swap(). Tried LV_COLOR_FORMAT_RGB565_SWAPPED
// (so blending happens directly in swapped format): solid fills
// rendered correctly but anti-aliased text became pixel noise --
// the v9.2 SWAPPED blend path has a bug with text glyphs.
#define LV_DRAW_SW_SUPPORT_RGB565         1

// ---- Memory (LVGL's internal heap) ----
// 48 KB is comfortable for the sanity demo (a few buttons + labels).
// We have 8 MB PSRAM available so we can grow this freely later.
#define LV_MEM_SIZE          (48U * 1024U)

// ---- Logging via Serial.printf (LV_LOG_PRINTF -> printf -> CDC) ----
#define LV_USE_LOG           1
#define LV_LOG_LEVEL         LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF        1

// ---- Fonts: enable a few sizes the sanity UI uses. ----
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// ---- Disable bundled demos (we ship our own UI) ----
#define LV_USE_DEMO_WIDGETS  0
#define LV_USE_DEMO_BENCHMARK 0

// ---- Performance overlays (turn on later if we want fps/HUD) ----
#define LV_USE_PERF_MONITOR  0
#define LV_USE_MEM_MONITOR   0
