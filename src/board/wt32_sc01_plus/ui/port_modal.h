#pragma once

#include "core/pin_port.h"

// Shared "Port B is in the wrong mode -- switch?" modal.
//
// Multiple section screens need Port B in a specific mode (battery=I2C,
// servo/motor=PWM, ELRS=UART, etc). Rather than each screen drawing
// its own modal, they all call PortModal::ensureMode() and an action
// continuation gets invoked once the port lands in the requested mode.
//
// The modal sits on lv_screen_active() at y=24 (below the status bar),
// matching the convention of the WiFi-edit modal in Settings.
//
// Continuation is a plain function pointer (no captures) -- handlers
// pull whatever they need from static state. Pass nullptr if you just
// want the port-switch and don't have a follow-up action.

namespace PortModal {

using Continuation = void (*)();

// If currentMode == needed: invokes onReady (if non-null) immediately
//   and returns true.
// Otherwise: builds + shows a modal asking the user to confirm the
//   switch. Tapping "Switch" releases the port, acquires the requested
//   mode under the given owner_label, persists it as preferred, then
//   invokes onReady. Tapping "Cancel" closes the modal without acting
//   and onReady is NOT called. Returns false in this case (caller
//   should consider the action deferred or skipped).
//
// Owner label is what shows up in /api/port/status.owner once the
// switch completes (e.g. "servo", "battery", "elrs").
bool ensureMode(PortMode needed, const char *owner_label, Continuation onReady);

// Close any open modal (idempotent). Call from screen cleanup paths
// so a stale modal can't survive the panel teardown.
void close();

// True if the modal is currently visible. Useful to avoid stacking
// re-prompts.
bool isOpen();

} // namespace PortModal
