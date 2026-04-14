#pragma once
#include <esp_task_wdt.h>

// Call periodically from long-running loops to avoid watchdog reset
static inline void feed_wdt() { esp_task_wdt_reset(); }
