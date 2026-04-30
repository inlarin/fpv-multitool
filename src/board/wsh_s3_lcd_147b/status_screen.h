#pragma once

// Waveshare status screen -- the Waveshare 172x320 LCD shows a
// non-interactive readout of the same kind of state the SC01 Plus puts
// in its 24px status bar (WiFi/IP, uptime, free heap, USB descriptor
// mode, Port B mode, OTA state, FW version).
//
// No menu, no per-app screens. All actual control happens via the web
// UI on this board. The LCD is purely a "is this thing alive and what
// is its IP" indicator.
//
// Public API:
//   init() -- one-shot draw of static chrome (title, labels, dividers)
//   tick() -- call from loop(); refreshes value fields at ~1 Hz, no-op
//             on the cycles between.

namespace StatusScreen {
    void init();
    void tick();
}
