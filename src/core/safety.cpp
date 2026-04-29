#include "safety.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

namespace Safety {

// ---- NVS keys ---------------------------------------------------------------
// Namespace name kept short (NVS limit is 15 chars). One key for the
// boot counter; the OTA app-valid state is tracked by the bootloader in
// the otadata partition, NOT here -- we just expose its current value.
static constexpr const char *NS               = "safety";
static constexpr const char *KEY_BOOT_COUNT   = "boot_count";

// ---- Module state -----------------------------------------------------------
static bool                       s_safe_mode             = false;
static bool                       s_validated             = false;
static uint32_t                   s_boot_count_at_start   = 0;
static uint32_t                   s_setup_started_ms      = 0;
static const esp_partition_t     *s_running_part          = nullptr;
static esp_ota_img_states_t       s_ota_state             = ESP_OTA_IMG_VALID;
static esp_reset_reason_t         s_reset_reason          = ESP_RST_UNKNOWN;

// ---- Internal helpers -------------------------------------------------------
static const char *otaStateName(esp_ota_img_states_t s) {
    switch (s) {
        case ESP_OTA_IMG_NEW:            return "NEW";
        case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
        case ESP_OTA_IMG_VALID:          return "VALID";
        case ESP_OTA_IMG_INVALID:        return "INVALID";
        case ESP_OTA_IMG_ABORTED:        return "ABORTED";
        case ESP_OTA_IMG_UNDEFINED:      return "UNDEFINED";
        default:                         return "?";
    }
}

static const char *resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT_PIN";
        case ESP_RST_SW:        return "SW_RESTART";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

// ---- Public API -------------------------------------------------------------

void earlyBootCheck() {
    s_setup_started_ms = millis();
    s_reset_reason     = esp_reset_reason();

    // Read + increment the boot counter. This MUST happen before any
    // potentially-crashy subsystem init -- if init crashes and we never
    // reach tickValidation, the counter stays incremented and the next
    // boot picks up where this one left off.
    Preferences p;
    if (p.begin(NS, false)) {
        s_boot_count_at_start = p.getUInt(KEY_BOOT_COUNT, 0);
        p.putUInt(KEY_BOOT_COUNT, s_boot_count_at_start + 1);
        p.end();
    }

    if (s_boot_count_at_start >= SAFE_MODE_BOOT_THRESHOLD) {
        s_safe_mode = true;
    }

    // Snapshot the current OTA partition state so /api/health and the
    // Serial banner can report it. The bootloader has already done its
    // NEW -> PENDING_VERIFY transition by the time we're here.
    s_running_part = esp_ota_get_running_partition();
    if (s_running_part) {
        if (esp_ota_get_state_partition(s_running_part, &s_ota_state) != ESP_OK) {
            s_ota_state = ESP_OTA_IMG_UNDEFINED;
        }
    }

    Serial.println();
    Serial.println(F("================ Safety boot check ================"));
    Serial.printf("  reset_reason : %s\n", resetReasonName(s_reset_reason));
    Serial.printf("  ota_partition: %s\n",
                  s_running_part ? s_running_part->label : "<unknown>");
    Serial.printf("  ota_state    : %s\n", otaStateName(s_ota_state));
    Serial.printf("  boot_count   : %u  (threshold %u for safe mode)\n",
                  (unsigned)s_boot_count_at_start, (unsigned)SAFE_MODE_BOOT_THRESHOLD);
    if (s_safe_mode) {
        Serial.println(F("  >>> SAFE MODE entered (too many failed boots) <<<"));
        Serial.println(F("  Skipping LCD/touch/UI subsystems -- only WiFi + WebServer."));
        Serial.println(F("  Reflash via /api/ota/upload to recover."));
    }
    Serial.println(F("===================================================="));
}

bool isSafeMode() { return s_safe_mode; }

void tickValidation(uint32_t threshold_ms) {
    if (s_validated) return;
    if (millis() - s_setup_started_ms < threshold_ms) return;

    // We've been alive long enough -- mark the running app as valid (so
    // the bootloader stops watching for rollback) AND zero the boot
    // counter so the NEXT boot starts clean.
    if (s_ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        Serial.printf("[safety] esp_ota_mark_app_valid_cancel_rollback() -> %s\n",
                      err == ESP_OK ? "OK (rollback canceled)" : esp_err_to_name(err));
        if (err == ESP_OK) s_ota_state = ESP_OTA_IMG_VALID;
    }

    Preferences p;
    if (p.begin(NS, false)) {
        p.putUInt(KEY_BOOT_COUNT, 0);
        p.end();
    }
    Serial.printf("[safety] boot validated after %u ms uptime -- counter reset\n",
                  (unsigned)threshold_ms);

    s_validated = true;
}

void markValidNow() {
    if (s_ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        s_ota_state = ESP_OTA_IMG_VALID;
    }
    Preferences p;
    if (p.begin(NS, false)) {
        p.putUInt(KEY_BOOT_COUNT, 0);
        p.end();
    }
    s_validated = true;
}

const char* otaStateStr()           { return otaStateName(s_ota_state); }
uint32_t    bootCount()             { return s_boot_count_at_start; }
bool        wasValidatedThisBoot()  { return s_validated; }
const char* lastResetReasonStr()    { return resetReasonName(s_reset_reason); }

} // namespace Safety
