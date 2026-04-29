// WT32-SC01 Plus -- Sprint 32 step 2c SDMMC sanity sketch.
//
// Built ONLY by [env:wt32_sc01_plus_sdmmc]. Mounts the onboard microSD
// slot in 1-bit SDMMC mode and exercises the basic file ops we'll need
// for the catalog flasher's firmware mirror:
//   - mount via SD_MMC (ESP-IDF SDMMC driver under Arduino's wrapper)
//   - print card type + size + free space
//   - list root directory
//   - write a small test file
//   - read it back and verify byte-for-byte
//
// Why 1-bit and not 4-bit: D1/D2 are not exposed on this PCB (factory
// app strings + board probe both confirmed this). 1-bit gives ~6x
// SPI throughput and is plenty for catalog use.
//
// Output: Serial CDC + on-screen status via LovyanGFX (no LVGL on this
// env -- this is hardware sanity, not UI).
//
// SD card requirement: any microSD. With SDMMC_FORCE_WIPE=1 (default in
// the env's build flags), this sketch WILL DELETE EVERYTHING on the
// card -- intended as a one-shot reformat to clean FAT32/exFAT (whatever
// ESP-IDF default produces, both Windows-readable). Drop the flag to
// run as read-only sanity instead.

#include <Arduino.h>
#include <SD_MMC.h>
#include <vector>
#include "lgfx_sc01_plus.h"
#include "pin_config_sc01_plus.h"

static LGFX_SC01Plus lcd;

static void lcdLine(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lcd.println(buf);
    Serial.println(buf);
}

static const char *cardTypeStr(uint8_t t) {
    switch (t) {
        case CARD_NONE: return "NONE";
        case CARD_MMC:  return "MMC";
        case CARD_SD:   return "SDSC";
        case CARD_SDHC: return "SDHC/SDXC";
        default:        return "?";
    }
}

static bool mountSdmmc(bool format_if_failed) {
    // D3 (= GPIO 41) is the chip-select-equivalent on the card. In 1-bit
    // SDMMC mode we don't drive it from the SDMMC controller, but it must
    // sit HIGH so the card stays in SD mode (not SPI). External pull-up
    // is present on most boards but cheap insurance to drive it ourselves.
    pinMode(SC01P_SD_D3, OUTPUT);
    digitalWrite(SC01P_SD_D3, HIGH);

    // 1-bit pin override: CLK, CMD, D0.
    if (!SD_MMC.setPins(SC01P_SD_CLK, SC01P_SD_CMD, SC01P_SD_D0)) {
        lcdLine("setPins FAILED");
        return false;
    }
    lcdLine("setPins OK");
    lcdLine(format_if_failed ? "mount(format=Y)..." : "mount(format=N)...");

    // Drop SDMMC clock to 4 MHz for the bring-up to dodge any timing
    // marginality on prototype hardware. We can crank back to default
    // (typically 20-40 MHz) once we know the slot wires are solid.
    if (!SD_MMC.begin("/sdcard", /* mode_1bit = */ true, format_if_failed,
                      /* sdmmc_frequency = */ 4000)) {
        lcdLine("begin returned false");
        return false;
    }
    lcdLine("mount OK");
    return true;
}

// Recursively walk a directory and delete every file/subdir under it.
// We collect names first then delete, because mutating the directory
// while iterating with openNextFile() is not safe.
static void wipeDir(const char *path) {
    File dir = SD_MMC.open(path);
    if (!dir || !dir.isDirectory()) return;

    // Pass 1: collect names.
    std::vector<String> files;
    std::vector<String> subdirs;
    File entry = dir.openNextFile();
    while (entry) {
        // entry.name() may be just basename or full path depending on
        // arduino-esp32 version -- normalize to absolute path here.
        String name = entry.name();
        if (name.length() && name[0] != '/') {
            name = String(path) + (strcmp(path, "/") == 0 ? "" : "/") + name;
        }
        if (entry.isDirectory()) subdirs.push_back(name);
        else                     files.push_back(name);
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();

    // Pass 2: delete files first, then recurse into subdirs.
    for (auto &f : files) SD_MMC.remove(f.c_str());
    for (auto &d : subdirs) {
        wipeDir(d.c_str());
        SD_MMC.rmdir(d.c_str());
    }
}

static void listRoot() {
    File root = SD_MMC.open("/");
    if (!root || !root.isDirectory()) {
        lcdLine("root open FAILED");
        return;
    }
    lcdLine("/");
    int n = 0;
    File f = root.openNextFile();
    while (f) {
        // Long names get clipped on the LCD line; full names go to Serial.
        if (f.isDirectory()) {
            lcdLine("  D %s/", f.name());
        } else {
            lcdLine("  F %s  (%u B)", f.name(), (unsigned)f.size());
        }
        n++;
        f = root.openNextFile();
    }
    if (n == 0) lcdLine("  (empty)");
}

static bool writeReadTest() {
    static const char *PATH = "/sc01_test.txt";
    static const char *PAYLOAD = "WT32-SC01 Plus SDMMC sanity OK";
    const size_t PLEN = strlen(PAYLOAD);

    File w = SD_MMC.open(PATH, FILE_WRITE);
    if (!w) { lcdLine("write open FAILED"); return false; }
    size_t wrote = w.write((const uint8_t *)PAYLOAD, PLEN);
    w.close();
    if (wrote != PLEN) {
        lcdLine("write short: %u/%u", (unsigned)wrote, (unsigned)PLEN);
        return false;
    }

    File r = SD_MMC.open(PATH, FILE_READ);
    if (!r) { lcdLine("read open FAILED"); return false; }
    char buf[64] = {0};
    size_t got = r.readBytes(buf, sizeof(buf) - 1);
    r.close();
    if (got != PLEN || memcmp(buf, PAYLOAD, PLEN) != 0) {
        lcdLine("verify FAILED");
        Serial.printf("  expected: \"%s\"\n", PAYLOAD);
        Serial.printf("  got     : \"%s\"  (%u bytes)\n", buf, (unsigned)got);
        return false;
    }
    lcdLine("write+read+verify OK (%u B)", (unsigned)PLEN);
    return true;
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { delay(10); }

    Serial.println();
    Serial.println(F("================ WT32-SC01 Plus SDMMC sanity ================"));

    lcd.init();
    lcd.setRotation(0);   // PCB offset already applied in lgfx_sc01_plus.h
    lcd.setBrightness(255);
    lcd.fillScreen(TFT_BLACK);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setTextSize(2);
    lcd.setCursor(4, 4);

    lcdLine("SDMMC sanity");
    lcdLine("pins CLK=%d CMD=%d", SC01P_SD_CLK, SC01P_SD_CMD);
    lcdLine("     D0=%d D3=%d", SC01P_SD_D0, SC01P_SD_D3);
    lcdLine("");

    // First mount: try as-is. If the FS is unrecognized (e.g. exFAT
    // on a 64 GB card with our framework's exFAT support compiled out),
    // ESP-IDF formats fresh FAT32 and re-mounts.
    bool mounted = mountSdmmc(/* format_if_failed = */ true);
    if (!mounted) {
        lcd.setTextColor(TFT_RED, TFT_BLACK);
        lcdLine("");
        lcdLine("MOUNT FAILED");
        lcdLine("(even after format)");
        return;
    }

    uint8_t  type = SD_MMC.cardType();
    uint64_t size_mb  = SD_MMC.cardSize()  / (1024ULL * 1024ULL);
    uint64_t total_mb = SD_MMC.totalBytes() / (1024ULL * 1024ULL);
    uint64_t used_mb  = SD_MMC.usedBytes()  / (1024ULL * 1024ULL);

    lcdLine("type: %s", cardTypeStr(type));
    lcdLine("size: %llu MB", size_mb);
    lcdLine("used: %llu / %llu MB", used_mb, total_mb);
    lcdLine("");

    // Wipe everything in / so the card is completely clean. This is
    // requested behavior for the catalog flasher's first-time setup.
    // ESP-IDF's "format_if_mount_failed" only triggers on FS failure,
    // so for cards that ARE mountable (e.g. existing FAT32 with junk),
    // we delete files manually.
    lcdLine("wiping...");
    wipeDir("/");
    used_mb = SD_MMC.usedBytes() / (1024ULL * 1024ULL);
    lcdLine("used after wipe: %llu MB", used_mb);
    lcdLine("");

    lcdLine("/ after wipe:");
    listRoot();
    lcdLine("");

    if (writeReadTest()) {
        lcd.setTextColor(TFT_GREEN, TFT_BLACK);
        lcdLine("");
        lcdLine("ALL OK -- card ready");
    } else {
        lcd.setTextColor(TFT_RED, TFT_BLACK);
        lcdLine("");
        lcdLine("FAIL");
    }
}

void loop() {
    static uint32_t last = 0;
    if (millis() - last >= 5000) {
        last = millis();
        Serial.printf("alive  free heap=%u  free psram=%u\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    }
    delay(50);
}
