// Phase 1 placeholders for every BoardApp tab. Each one renders a title
// + a 1-line description so we can tap through the bottom-bar nav and
// confirm the routing works. Real screens replace these one by one in
// Phase 2+.
//
// To replace a placeholder: move its build function to its own
// screen_<name>.cpp, drop the implementation here, and the linker
// picks up the new definition (one defn per name).

#include "screens.h"
#include <lvgl.h>

namespace {

// Common look-and-feel for the placeholder body.
void buildPlaceholder(lv_obj_t *tab, const char *title, const char *body) {
    lv_obj_set_style_bg_color(tab, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_pad_all(tab, 12, 0);

    lv_obj_t *t = lv_label_create(tab);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 4);

    lv_obj_t *b = lv_label_create(tab);
    lv_label_set_text(b, body);
    lv_obj_set_style_text_color(b, lv_color_hex(0xa0a0a0), 0);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 0, 44);
}

} // namespace

namespace screens {

void buildHome(lv_obj_t *tab) {
    buildPlaceholder(tab,
        "Home",
        "Dashboard.\nIP, uptime, free heap, port-B mode, "
        "WiFi RSSI, motor armed indicator -- coming in Phase 2.");
}

void buildServo(lv_obj_t *tab) {
    buildPlaceholder(tab,
        "Servo",
        "PWM servo tester.\nBig pulse-us slider, sweep mode, "
        "endpoint markers -- Phase 2.");
}

void buildMotor(lv_obj_t *tab) {
    buildPlaceholder(tab,
        "Motor",
        "DShot ESC + telemetry.\nArm/Disarm, throttle slider, "
        "live RPM/temp/current chart -- Phase 2.");
}

void buildBattery(lv_obj_t *tab) {
    buildPlaceholder(tab,
        "Battery",
        "DJI / Autel / clone batteries.\nQuick read, cycles reset, "
        "DF editor, MAC catalog -- Phase 2.");
}

void buildElrs(lv_obj_t *tab) {
    buildPlaceholder(tab,
        "ELRS RX",
        "Receiver tools.\nLink monitor (RSSI / LQ / SNR / channels), "
        "bind, parameter edit, in-place patch -- Phase 2.");
}

void buildSniff(lv_obj_t *tab) {
    buildPlaceholder(tab,
        "RC Sniff",
        "SBUS / iBus / PPM frame sniffer.\n16-channel chart, "
        "frame rate, CRC errors -- Phase 2.");
}

void buildCatalog(lv_obj_t *tab) {
    buildPlaceholder(tab,
        "Catalog",
        "ELRS firmware presets on SD.\nVendor / chip / version "
        "list -> one-tap flash -- Phase 3 (the big one).");
}

void buildSettings(lv_obj_t *tab) {
    buildPlaceholder(tab,
        "Settings",
        "WiFi config, display rotation, touch recalibrate, "
        "USB descriptor mode, brightness -- Phase 4.");
}

} // namespace screens
