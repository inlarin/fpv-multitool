#pragma once

// Lightweight Serial CLI for the WT32-SC01 Plus full app.
//
// Polled from the main loop -- consumes one line at a time and dispatches
// to the built-in command handlers below. Built-ins:
//
//   reboot                  -- normal reset (esp_restart)
//   reboot bootloader       -- reset INTO ROM download mode (esptool can
//                              flash without BOOT button -- this is the
//                              "always-can-reflash" escape hatch)
//   wifi set <ssid> <pass>  -- save creds to NVS (BoardSettings) and
//                              reconnect immediately
//   wifi show               -- print current ssid (not pass)
//   wifi clear              -- wipe creds from NVS
//   wifi reconnect          -- bounce STA with current NVS creds
//   help                    -- list commands
//
// The CLI is intentionally stateless and tiny -- no allocator, fixed
// 200-byte line buffer. Every reset/wifi command is a one-liner,
// nothing requires shell history or auto-complete.

namespace SerialCli {

void begin();   // optional, prints banner
void poll();    // call from main loop -- non-blocking, drains pending bytes

}
