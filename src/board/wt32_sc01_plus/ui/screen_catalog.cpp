// Catalog screen -- on-device firmware library browser, on top of the
// SD_MMC slot. Phase 1: read-only browser of /catalog/<protocol>/<vendor>/
// /<side>/<device>/manifest.json -- full one-tap-flash drilldown is
// queued for Phase 3 once the SDMMC mount is wired into the main loop
// (Sprint 32 step 2c). Until then the screen behaves gracefully: if the
// mount fails, the diagnostic explains why, and the existing web flasher
// remains the path to actually flash.
//
// Layout (320x408 panel, scrollable column):
//   Status card   : SD state / card type / size / catalog dir count
//   Hint card     : guidance pointing at /api/flash/* until Phase 3 lands
//   Refresh row   : retry-mount button
//
// SD pins (1-bit SDMMC, see pin_config_sc01_plus.h):
//   CLK = 39   CMD = 40   D0 = 38   D3 (CS-equiv held HIGH) = 41
// None of these collide with Port B, so PinPort isn't involved.

#include "screens.h"

#include <Arduino.h>
#include <SD_MMC.h>

#include "pin_config.h"
#include "safety.h"

namespace screens {

// ---- Live widgets ---------------------------------------------------------

static lv_obj_t *s_state_lbl    = nullptr;
static lv_obj_t *s_type_lbl     = nullptr;
static lv_obj_t *s_size_lbl     = nullptr;
static lv_obj_t *s_catalog_lbl  = nullptr;
static lv_obj_t *s_listing      = nullptr;
static bool     s_mount_ok      = false;

// Track our own mount state -- `SD_MMC.cardType() != CARD_NONE` only
// flips after a successful begin(), so we use it as the live probe.

// ---- Helpers --------------------------------------------------------------

static const char *cardTypeStr(uint8_t t) {
    switch (t) {
        case CARD_NONE: return "no card";
        case CARD_MMC:  return "MMC";
        case CARD_SD:   return "SDSC";
        case CARD_SDHC: return "SDHC/SDXC";
        default:        return "unknown";
    }
}

static bool tryMount() {
    pinMode(SC01P_SD_D3, OUTPUT);
    digitalWrite(SC01P_SD_D3, HIGH);
    if (!SD_MMC.setPins(SC01P_SD_CLK, SC01P_SD_CMD, SC01P_SD_D0)) {
        Safety::logf("[catalog] SD_MMC.setPins failed");
        return false;
    }
    if (!SD_MMC.begin("/sdcard", /*mode_1bit=*/true,
                      /*format_if_mount_failed=*/false,
                      /*sdmmc_frequency=*/4000)) {
        Safety::logf("[catalog] SD_MMC.begin failed");
        return false;
    }
    return true;
}

// Pretty-print bytes as KB/MB/GB.
static void formatBytes(uint64_t b, char *out, size_t n) {
    if (b >= (uint64_t)1024 * 1024 * 1024) {
        snprintf(out, n, "%.2f GB", b / (1024.0 * 1024.0 * 1024.0));
    } else if (b >= 1024 * 1024) {
        snprintf(out, n, "%.1f MB", b / (1024.0 * 1024.0));
    } else if (b >= 1024) {
        snprintf(out, n, "%.1f KB", b / 1024.0);
    } else {
        snprintf(out, n, "%lu B", (unsigned long)b);
    }
}

// Walk one directory level. Appends each entry as a label row inside `parent`.
// Returns the count of entries seen (used by status card).
static int listDirInto(const char *path, lv_obj_t *parent, int max_rows = 32) {
    File dir = SD_MMC.open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return -1;
    }
    int count = 0;
    while (true) {
        File f = dir.openNextFile();
        if (!f) break;
        if (count < max_rows) {
            lv_obj_t *l = lv_label_create(parent);
            lv_obj_set_width(l, lv_pct(100));
            lv_obj_set_style_text_color(l, lv_color_hex(0xE0E0E0), 0);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
            char line[80];
            if (f.isDirectory()) {
                snprintf(line, sizeof(line), "[ %s ]", f.name());
            } else {
                snprintf(line, sizeof(line), "  %s  (%lu B)",
                         f.name(), (unsigned long)f.size());
            }
            lv_label_set_text(l, line);
        }
        count++;
        f.close();
    }
    dir.close();
    return count;
}

// ---- Card / row builders --------------------------------------------------

static lv_obj_t *makeCard(lv_obj_t *parent, const char *title) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1a1f24), LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0x394150), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(card, 4, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    if (title) {
        lv_obj_t *hdr = lv_label_create(card);
        lv_obj_set_style_text_color(hdr, lv_color_hex(0x2E86AB), 0);
        lv_obj_set_style_text_font(hdr, &lv_font_montserrat_18, 0);
        lv_label_set_text(hdr, title);
    }
    return card;
}

static lv_obj_t *makeKvRow(lv_obj_t *parent, const char *key) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *k = lv_label_create(row);
    lv_obj_set_style_text_color(k, lv_color_hex(0xa0a0a0), 0);
    lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);
    lv_label_set_text(k, key);

    lv_obj_t *v = lv_label_create(row);
    lv_obj_set_style_text_color(v, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_label_set_text(v, "...");
    return v;
}

// ---- Refresh ---------------------------------------------------------------

static void refresh() {
    if (!s_state_lbl) return;

    s_mount_ok = (SD_MMC.cardType() != CARD_NONE) || tryMount();

    if (!s_mount_ok) {
        lv_label_set_text(s_state_lbl, "no card / mount failed");
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0xE6A23C), 0);
        lv_label_set_text(s_type_lbl,    "...");
        lv_label_set_text(s_size_lbl,    "...");
        lv_label_set_text(s_catalog_lbl, "...");
        // Wipe any prior listing rows so a stale tree doesn't survive an
        // SD-eject -> tap Refresh cycle.
        if (s_listing) lv_obj_clean(s_listing);
        return;
    }

    lv_label_set_text(s_state_lbl, "MOUNTED");
    lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0x06A77D), 0);

    char buf[40];
    lv_label_set_text(s_type_lbl, cardTypeStr(SD_MMC.cardType()));
    formatBytes(SD_MMC.cardSize(), buf, sizeof(buf));
    lv_label_set_text(s_size_lbl, buf);

    if (s_listing) lv_obj_clean(s_listing);
    int n = listDirInto("/catalog", s_listing);
    if (n < 0) {
        lv_label_set_text(s_catalog_lbl, "no /catalog dir");
        lv_obj_t *l = lv_label_create(s_listing);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_color(l, lv_color_hex(0xa0a0a0), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_label_set_text(l, "(create /catalog/<protocol>/<vendor>/...\n"
                             " or sync from web flasher in Phase 3)");
    } else {
        snprintf(buf, sizeof(buf), "%d entr%s", n, n == 1 ? "y" : "ies");
        lv_label_set_text(s_catalog_lbl, buf);
    }
}

static void refreshClicked(lv_event_t * /*e*/) {
    Safety::logf("[catalog] manual refresh");
    refresh();
}

// ---- Public entry ---------------------------------------------------------

void buildCatalog(lv_obj_t *panel) {
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(panel, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);

    // ---- Status card ----
    lv_obj_t *status = makeCard(panel, "SD Card");
    s_state_lbl   = makeKvRow(status, "State");
    s_type_lbl    = makeKvRow(status, "Type");
    s_size_lbl    = makeKvRow(status, "Size");
    s_catalog_lbl = makeKvRow(status, "/catalog");

    // ---- Refresh button ----
    lv_obj_t *btn = lv_button_create(panel);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 44);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x394150), LV_PART_MAIN);
    lv_obj_add_event_cb(btn, refreshClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Refresh / re-mount");
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_center(bl);

    // ---- Listing card (filled by refresh) ----
    s_listing = makeCard(panel, "Contents");
    lv_obj_set_height(s_listing, 200);
    lv_obj_set_scroll_dir(s_listing, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_listing, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_listing, LV_OBJ_FLAG_SCROLLABLE);

    refresh();
}

} // namespace screens
