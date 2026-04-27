#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Shared state between web server and hardware modules
// Web handlers set commands, hardware modules execute them
// All access must go through Lock guard or take/give explicitly
namespace WebState {

// ===== Mutex for thread-safe access =====
extern SemaphoreHandle_t mutex;

void initMutex();

// RAII guard. Mutex is recursive — nested Lock from the same task is
// safe. Pair with xSemaphoreTakeRecursive/GiveRecursive.
class Lock {
public:
    Lock()  { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGiveRecursive(mutex); }
};

// ===== Servo =====
struct ServoState {
    bool active = false;
    int pulseUs = 1500;
    int freq = 50;
    bool sweep = false;
    // Sweep config + range recorder
    int sweepMinUs = 1000;
    int sweepMaxUs = 2000;
    int sweepPeriodMs = 2000;  // time for full min->max->min cycle
    int markedMinUs = 0;       // user-captured endpoints (0 = not set)
    int markedMaxUs = 0;
    int observedMinUs = 0;     // commanded pulse extremes observed this session
    int observedMaxUs = 0;
};
extern ServoState servo;

// ===== Motor =====
struct MotorState {
    bool armed = false;
    bool armRequest = false;
    bool disarmRequest = false;
    bool beepRequest = false;
    int  beepCmd = 1;  // DShot beep variant 1..5 (BEACON1..BEACON5)
    int throttle = 0;      // 0-2000
    int maxThrottle = 2000; // safety cap (0-2000)
    int dshotSpeed = 300;  // 150/300/600
    bool dirCwRequest = false;
    bool dirCcwRequest = false;
    bool mode3DOnRequest = false;
    bool mode3DOffRequest = false;
};
extern MotorState motor;

// ===== Battery (read by UI thread, displayed via WebSocket) =====
// Uses DJIBattery::readAll() directly from web thread (I2C)

// ===== Battery service (executed in web thread) =====
// Migrated to typed Facade 2026-04-27. Direct field access still works.
enum BattServiceAction { BS_NONE, BS_UNSEAL, BS_CLEARPF, BS_SEAL };
struct BattServiceState {
    BattServiceAction pending = BS_NONE;
    String lastResult;

    // Typed accessors. requestAction is what HTTP routes call to enqueue;
    // takePending is what the main loop calls to atomically pop the queue.
    // markResult sets the result string, lastResult atomically clears
    // pending after the action completes.
    void requestAction(BattServiceAction a) { Lock lock; pending = a; }
    BattServiceAction takePending() {
        Lock lock;
        BattServiceAction a = pending;
        pending = BS_NONE;
        return a;
    }
    void markResult(const char *s) { Lock lock; lastResult = s; }
    String getResult() const       { Lock lock; return lastResult; }
    BattServiceAction getPending() const { Lock lock; return pending; }
};
extern BattServiceState battSvc;

// ===== CRSF / ELRS telemetry state =====
// Typed Facade migration step 1 (2026-04-27): public bool fields are now
// thin wrappers that take Lock internally. Direct `WebState::crsf.enabled`
// access still works during transition, but new code should call the
// typed accessors. Drop direct fields once all subsystems migrate.
struct CRSFState {
    bool enabled = false;       // true = service running
    bool inverted = false;      // UART signal inversion (for inverted CRSF FCs)

    // Typed accessors — each takes Lock internally. Recursive mutex so
    // callers that hold their own Lock don't deadlock.
    bool isEnabled() const   { Lock lock; return enabled; }
    bool isInverted() const  { Lock lock; return inverted; }
    void setEnabled(bool v)  { Lock lock; enabled = v; }
    void setInverted(bool v) { Lock lock; inverted = v; }
    // Atomic start/stop for the service lifecycle — sets both fields
    // under one lock so the WS broadcast can't observe inconsistency.
    void markStarted(bool was_inverted) { Lock lock; enabled = true; inverted = was_inverted; }
    void markStopped()                  { Lock lock; enabled = false; }
};
extern CRSFState crsf;

// ===== ELRS flash state =====
struct FlashState {
    uint8_t *fw_data = nullptr;     // firmware buffer (PSRAM)
    size_t fw_size = 0;
    size_t fw_received = 0;         // bytes received during upload
    bool flash_request = false;     // start flashing when true
    bool in_progress = false;
    int progress_pct = 0;
    String stage;                   // "Connecting", "Erasing", "Writing", "Done"
    String lastResult;              // final status message
    uint32_t flash_offset = 0;      // target offset (0 for full image, 0x10000 for app0)
    bool flash_raw = false;          // skip format detection (writes arbitrary blobs, e.g. partition tables)
    bool flash_stay = false;         // after write, keep RX in DFU (for chaining multiple flashes)
    bool flash_via_stub = false;     // use in-app ELRS stub flasher @ 420000 (no physical BOOT req'd)
};
extern FlashState flashState;

} // namespace WebState
