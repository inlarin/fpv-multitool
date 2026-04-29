#pragma once

#include <stdint.h>

class BoardDisplay;

// BoardApp -- owner of the LVGL UI on the WT32-SC01 Plus.
//
// Holds the bottom-bar tab navigation (Home / Servo / Motor / Battery /
// ELRS / Sniff / Catalog / Settings) and routes tap events to the
// per-screen builders. Phase 1 ships placeholder content in every tab;
// Phase 2+ replaces each placeholder with the real screen.
//
// Usage from main_sc01_plus.cpp:
//
//   BoardApp app;
//   app.begin(s_display);   // creates LVGL display + indev + UI tree
//   ...
//   for (;;) {
//       app.loop();          // wraps lv_timer_handler() + periodic UI work
//       ...
//   }

class BoardApp {
public:
    void begin(BoardDisplay &display);

    // Drive LVGL + any UI-side periodic refresh (status bar, tab badges).
    // Call from the main loop at ~5 ms cadence.
    void loop();

private:
    BoardDisplay *_display = nullptr;
};
