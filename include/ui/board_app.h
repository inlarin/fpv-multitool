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

    // Status-bar "working" pill: shows a spinner + label inline on the
    // top status bar. Use during long-running ops (OTA, flash, scan).
    // Pass nullptr/empty to clear. Thread-safe to call from event handlers.
    static void setBusy(const char *label, int progress_pct = -1);
    static void clearBusy();

    // Synthetic touch injection -- enqueue a press+release at (x, y).
    // Scheduled within the next few LVGL frames; the indev read callback
    // pulls from this queue BEFORE polling FT6336 hardware. Used by
    // /api/sys/ui/tap for off-board UI scripting (closes the screenshot
    // feedback loop). Returns false if the queue is full.
    static bool injectTap(int16_t x, int16_t y);

private:
    BoardDisplay *_display = nullptr;
};
