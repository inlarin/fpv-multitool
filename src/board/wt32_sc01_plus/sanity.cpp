// WT32-SC01 Plus -- Sprint 32 sanity sketch.
//
// Built ONLY by [env:wt32_sc01_plus_sanity]. Goal: confirm toolchain + QSPI
// PSRAM init + native USB CDC works on this board, before bolting on LCD /
// touch / SD. Periodically prints chip + memory + I2C scan.
//
// Expected output on first power-up:
//   - "ESP32-S3, 16 MB flash, 2 MB PSRAM (...)" lines from boot ROM (visible
//     via the same USB-CDC) followed by our once-per-2s status block
//   - I2C scan should find a touch controller on either 0x38 (FT6336) or
//     0x5D / 0x14 (GT911) -- this confirms the LCD/touch I2C wiring is sane

#include <Arduino.h>
#include <Wire.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_psram.h>
#include <esp_mac.h>

#include "pin_config_sc01_plus.h"

static void printChipInfo() {
    esp_chip_info_t info;
    esp_chip_info(&info);

    Serial.println();
    Serial.println(F("================ WT32-SC01 Plus sanity ================"));
    Serial.printf("  chip      : ESP32-S3 rev v%d.%d, %d cores\n",
                  info.revision / 100, info.revision % 100, info.cores);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        Serial.printf("  flash     : %u MB\n", flash_size / (1024U * 1024U));
    } else {
        Serial.println("  flash     : <esp_flash_get_size failed>");
    }

#ifdef BOARD_HAS_PSRAM
    size_t psram_total = esp_psram_get_size();
    Serial.printf("  PSRAM     : %u MB total, %u bytes free, %u bytes largest block\n",
                  (unsigned)(psram_total / (1024U * 1024U)),
                  (unsigned)ESP.getFreePsram(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    if (psram_total == 0) {
        Serial.println("  PSRAM     : !!! NOT DETECTED -- check qspi vs opi memory_type in PIO !!!");
    }
#else
    Serial.println("  PSRAM     : (BOARD_HAS_PSRAM not defined)");
#endif

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    Serial.printf("  MAC (STA) : %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    Serial.printf("  heap free : %u bytes\n", (unsigned)ESP.getFreeHeap());
    Serial.printf("  IDF       : %s\n", esp_get_idf_version());
}

// Scan one TwoWire bus, report anything that ACKs. FT6336 sits at 0x38;
// GT911 at 0x5D or 0x14 depending on INT/RST strap.
static int scanBus(TwoWire &bus, const char *label, int sda, int scl) {
    bus.begin(sda, scl, SC01P_TOUCH_FREQ_HZ);
    Serial.printf("  %s scan (SDA=%d SCL=%d): ", label, sda, scl);
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        bus.beginTransmission(addr);
        if (bus.endTransmission() == 0) {
            const char *hint = "";
            if (addr == SC01P_TOUCH_ADDR_FT6336) hint = " (FT6336 touch)";
            else if (addr == SC01P_TOUCH_ADDR_GT911A) hint = " (GT911 touch)";
            else if (addr == SC01P_TOUCH_ADDR_GT911B) hint = " (GT911 touch alt)";
            Serial.printf("0x%02X%s ", addr, hint);
            found++;
        }
    }
    if (found == 0) Serial.println("<nothing>");
    else            Serial.printf(" (%d device%s)\n", found, found == 1 ? "" : "s");
    return found;
}

// Release the shared LCD/touch RST line, give the touch IC time to boot,
// then scan both I2C peripherals -- on this board the touch wiring could
// be on either Wire (I2C0) or Wire1 (I2C1). Try both.
static void i2cScan() {
    static bool rst_released = false;
    if (!rst_released) {
        pinMode(SC01P_LCD_RST, OUTPUT);     // = SC01P_TOUCH_RST, shared
        digitalWrite(SC01P_LCD_RST, LOW);
        delay(20);
        digitalWrite(SC01P_LCD_RST, HIGH);
        delay(120);                          // FT6336 ~100 ms, GT911 ~50 ms
        rst_released = true;
        Serial.printf("  RST (GPIO %d) released HIGH\n", SC01P_LCD_RST);
    }
    scanBus(Wire,  "I2C bus 0 (Wire )", SC01P_TOUCH_SDA, SC01P_TOUCH_SCL);
    scanBus(Wire1, "I2C bus 1 (Wire1)", SC01P_TOUCH_SDA, SC01P_TOUCH_SCL);
}

void setup() {
    Serial.begin(115200);
    // Wait briefly for USB CDC to enumerate after reset; some hosts take a
    // beat. Don't block forever — if nobody is listening we still want
    // to run the loop.
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { delay(10); }

    printChipInfo();
    i2cScan();
    Serial.println(F("======================================================="));
}

void loop() {
    // Once per 2 s: re-print chip info + scan, mostly to prove the board
    // hasn't crashed and USB CDC stays open across iterations.
    static uint32_t last = 0;
    if (millis() - last >= 2000) {
        last = millis();
        printChipInfo();
        i2cScan();
        Serial.println(F("-------------------------------------------------------"));
    }
}
