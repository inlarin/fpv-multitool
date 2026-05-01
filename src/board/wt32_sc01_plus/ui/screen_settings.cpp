// Settings screen -- configuration UI replacing the Serial CLI.
//
// Sections:
//   Display  : rotation 0/1/2/3, brightness slider, touch recalibrate
//   Port B   : current mode + owner, switch buttons, force-release
//   WiFi     : current SSID + Edit button (modal w/ lv_keyboard for ssid/pass)
//   Beacon   : current url + interval + Edit button (modal w/ keyboard)
//   About    : fw / ip / uptime / heap / ota_state / boot_count + Reboot
//
// Pattern: scrollable lv_obj container holding section "cards"
// (rounded dark surface, 8 px padding). Each card has a 18 px Montserrat
// header label followed by widgets. Tap targets >= 48 px.
//
// Live updates (uptime / heap on the About card) ride a 1 Hz lv_timer.

#include "screens.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include "board_settings.h"
#include "board_display.h"
#include "safety.h"
#include "ui/board_app.h"
#include "core/pin_port.h"
#include "core/usb_mode.h"

namespace screens {

// Fires when the panel is deleted on Back. Kills only OUR timer +
// nulls static widget pointers. LVGL internal timers MUST be left alone.
static void settingsCleanup(lv_event_t *e);

// Defined further down (Port B section); aboutTick polls it once per
// second so the Mode/Owner badge tracks external state changes (web
// /api/port/preferred, another screen acquiring the port, etc.).
static void portBRefresh();

// ---- About section live-refresh -------------------------------------------

static lv_obj_t *s_about_uptime = nullptr;
static lv_obj_t *s_about_heap   = nullptr;
static lv_obj_t *s_about_ip     = nullptr;
static lv_obj_t *s_about_ota    = nullptr;
static lv_timer_t *s_about_timer = nullptr;

static void aboutTick(lv_timer_t * /*t*/) {
    if (!s_about_uptime) return;   // section was rebuilt / left

    // Piggyback the Port B card's live refresh on the same 1 Hz timer
    // -- mode/owner can change from web (POST /api/port/preferred) or
    // from another LVGL screen acquiring the port, so we re-poll here
    // rather than only on user taps.
    portBRefresh();

    char buf[40];

    uint32_t s = millis() / 1000;
    if (s < 60)        snprintf(buf, sizeof(buf), "%us", (unsigned)s);
    else if (s < 3600) snprintf(buf, sizeof(buf), "%um %us",
                                  (unsigned)(s / 60), (unsigned)(s % 60));
    else               snprintf(buf, sizeof(buf), "%uh %um",
                                  (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
    lv_label_set_text(s_about_uptime, buf);

    snprintf(buf, sizeof(buf), "%u KB free", (unsigned)(ESP.getFreeHeap() / 1024));
    lv_label_set_text(s_about_heap, buf);

    if (WiFi.status() == WL_CONNECTED) {
        lv_label_set_text(s_about_ip, WiFi.localIP().toString().c_str());
    } else if (WiFi.getMode() & WIFI_AP) {
        lv_label_set_text(s_about_ip, "AP 192.168.4.1");
    } else {
        lv_label_set_text(s_about_ip, "no link");
    }

    snprintf(buf, sizeof(buf), "%s (boot #%u)",
             Safety::otaStateStr(), (unsigned)Safety::bootCount());
    lv_label_set_text(s_about_ota, buf);
}

// ---- Card builder helper --------------------------------------------------

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
    lv_obj_set_style_pad_gap(card, 6, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(card);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x2E86AB), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_18, 0);
    lv_label_set_text(hdr, title);
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

// ---- Display section ------------------------------------------------------

static void rotationClicked(lv_event_t *e) {
    int rot = (int)(intptr_t)lv_event_get_user_data(e);
    BoardApp::display().setRotation((uint8_t)rot);
    // The active LVGL display needs to be told the new resolution and
    // the whole UI rebuilt, otherwise the Settings card keeps the old
    // dimensions and stale touch coords. Easiest reliable path is a
    // soft-reboot -- rotation is rare enough that the 2 s reboot delay
    // beats the complexity of live re-layout.
    Serial.printf("[settings] rotation -> %d, restarting to apply\n", rot);
    delay(50);
    esp_restart();
}

static void brightnessChanged(lv_event_t *e) {
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    BoardApp::display().setBrightness((uint8_t)v);
    // Persist to NVS so the level survives reboot. Cheap (single
    // putUChar) but called on every slider tick -- LVGL slider
    // throttles to ~10 events/s during a drag which the NVS layer
    // handles fine.
    BoardSettings::setBrightness((uint8_t)v);
}

static void recalibrateClicked(lv_event_t * /*e*/) {
    // Ditto -- recalibrate takes over the LCD with LovyanGFX's own
    // touch UI, which does not coexist with LVGL on the same screen.
    // Wipe NVS calibration + reboot; on next boot BoardDisplay sees
    // missing calibration and runs the interactive flow before LVGL
    // ever starts.
    BoardSettings::clearTouchCalibrate();
    Serial.println("[settings] touch cal wiped, rebooting to recalibrate");
    delay(50);
    esp_restart();
}

static void buildDisplaySection(lv_obj_t *parent) {
    lv_obj_t *card = makeCard(parent, "Display");

    // Rotation row -- 4 buttons in a row, current rotation highlighted.
    lv_obj_t *rot_row = lv_obj_create(card);
    lv_obj_remove_style_all(rot_row);
    lv_obj_set_width(rot_row, lv_pct(100));
    lv_obj_set_height(rot_row, 48);
    lv_obj_set_layout(rot_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(rot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(rot_row, 6, LV_PART_MAIN);
    lv_obj_set_flex_align(rot_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    uint8_t cur_rot = BoardApp::display().rotation();
    static const char *ROT_LBL[4] = { "0\xC2\xB0", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_button_create(rot_row);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, 44);
        bool active = (i == (int)cur_rot);
        lv_obj_set_style_bg_color(b,
            lv_color_hex(active ? 0x06A77D : 0x394150), LV_PART_MAIN);
        lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
        lv_obj_add_event_cb(b, rotationClicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(b);
        lv_label_set_text(lbl, ROT_LBL[i]);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);
    }

    // Brightness slider.
    lv_obj_t *br_lbl = lv_label_create(card);
    lv_label_set_text(br_lbl, "Brightness");
    lv_obj_set_style_text_color(br_lbl, lv_color_hex(0xa0a0a0), 0);
    lv_obj_set_style_text_font(br_lbl, &lv_font_montserrat_14, 0);

    lv_obj_t *sl = lv_slider_create(card);
    lv_obj_set_width(sl, lv_pct(100));
    lv_obj_set_height(sl, 28);
    lv_slider_set_range(sl, 32, 255);   // <32 makes the LCD hard to read
    lv_slider_set_value(sl, BoardSettings::brightness(), LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, brightnessChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    // Recalibrate touch button.
    lv_obj_t *rb = lv_button_create(card);
    lv_obj_set_width(rb, lv_pct(100));
    lv_obj_set_height(rb, 44);
    lv_obj_set_style_bg_color(rb, lv_color_hex(0x394150), LV_PART_MAIN);
    lv_obj_set_style_radius(rb, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(rb, recalibrateClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *rl = lv_label_create(rb);
    lv_label_set_text(rl, "Recalibrate touch (reboots)");
    lv_obj_set_style_text_color(rl, lv_color_white(), 0);
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_14, 0);
    lv_obj_center(rl);
}

// ---- USB descriptor mode section ------------------------------------------
//
// USB descriptor is fixed at enumeration time, so changing modes
// requires a reboot. The card shows what's running now (active) +
// what's saved as preferred (next boot). Tapping a mode pops a small
// confirm modal -- on Reboot the new mode is saved to NVS and
// esp_restart() fires.

static lv_obj_t *s_usb_active_lbl    = nullptr;
static lv_obj_t *s_usb_pref_lbl      = nullptr;
static lv_obj_t *s_usb_btns[3]       = { nullptr, nullptr, nullptr };
static lv_obj_t *s_usb_confirm_modal = nullptr;
static UsbDescriptorMode s_usb_pending = USB_MODE_CDC;

static const UsbDescriptorMode USB_MODES[3] = {
    USB_MODE_CDC, USB_MODE_USB2TTL, USB_MODE_USB2I2C,
};
static const char *USB_LABELS[3] = { "CDC", "USB2TTL", "USB2I2C" };

static void usbRefresh() {
    if (!s_usb_active_lbl) return;
    UsbDescriptorMode active = UsbMode::active();
    UsbDescriptorMode pref   = UsbMode::load();
    lv_label_set_text(s_usb_active_lbl, UsbMode::name(active));
    lv_label_set_text(s_usb_pref_lbl,   UsbMode::name(pref));
    // Highlight the button matching the *preferred* (next-boot) mode.
    for (int i = 0; i < 3; i++) {
        if (!s_usb_btns[i]) continue;
        bool active_btn = (USB_MODES[i] == pref);
        lv_obj_set_style_bg_color(s_usb_btns[i],
            lv_color_hex(active_btn ? 0x06A77D : 0x394150), LV_PART_MAIN);
    }
}

static void usbConfirmClose() {
    if (s_usb_confirm_modal) {
        lv_obj_delete(s_usb_confirm_modal);
        s_usb_confirm_modal = nullptr;
    }
}

static void usbConfirmReboot(lv_event_t * /*e*/) {
    Safety::logf("[settings] USB mode -> %s, rebooting",
                 UsbMode::name(s_usb_pending));
    UsbMode::switchAndReboot(s_usb_pending);   // saves NVS + esp_restart
}

static void usbConfirmCancel(lv_event_t * /*e*/) {
    usbConfirmClose();
}

static void usbModeClicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= 3) return;
    UsbDescriptorMode want = USB_MODES[idx];

    if (want == UsbMode::load() && want == UsbMode::active()) {
        // Already there both at runtime AND in NVS, nothing to do.
        return;
    }

    s_usb_pending = want;
    if (s_usb_confirm_modal) return;   // already open

    s_usb_confirm_modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_usb_confirm_modal, 320, 480 - 24);
    lv_obj_set_pos(s_usb_confirm_modal, 0, 24);
    lv_obj_set_style_bg_color(s_usb_confirm_modal, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_usb_confirm_modal, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_usb_confirm_modal, 16, LV_PART_MAIN);
    lv_obj_set_layout(s_usb_confirm_modal, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_usb_confirm_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_usb_confirm_modal, 12, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(s_usb_confirm_modal);
    lv_label_set_text(title, "Switch USB mode");
    lv_obj_set_style_text_color(title, lv_color_hex(0xE6A23C), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);

    lv_obj_t *msg = lv_label_create(s_usb_confirm_modal);
    lv_obj_set_width(msg, lv_pct(100));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    char buf[180];
    snprintf(buf, sizeof(buf),
             "USB descriptor is fixed at enumeration. To switch from %s "
             "to %s the board must reboot. WiFi will reconnect in ~5 s. "
             "Continue?",
             UsbMode::name(UsbMode::active()), UsbMode::name(want));
    lv_label_set_text(msg, buf);

    lv_obj_t *spacer = lv_obj_create(s_usb_confirm_modal);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_size(spacer, 0, 1);

    lv_obj_t *btn_row = lv_obj_create(s_usb_confirm_modal);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, 56);
    lv_obj_set_layout(btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(btn_row, 8, LV_PART_MAIN);

    lv_obj_t *cancel = lv_button_create(btn_row);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_set_height(cancel, 52);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x394150), LV_PART_MAIN);
    lv_obj_set_style_radius(cancel, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel, usbConfirmCancel, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_18, 0);
    lv_obj_center(cl);

    lv_obj_t *go = lv_button_create(btn_row);
    lv_obj_set_flex_grow(go, 1);
    lv_obj_set_height(go, 52);
    lv_obj_set_style_bg_color(go, lv_color_hex(0xE63946), LV_PART_MAIN);   // red -- destructive (reboot)
    lv_obj_set_style_radius(go, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(go, usbConfirmReboot, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *gl = lv_label_create(go);
    lv_label_set_text(gl, "Reboot");
    lv_obj_set_style_text_color(gl, lv_color_white(), 0);
    lv_obj_set_style_text_font(gl, &lv_font_montserrat_18, 0);
    lv_obj_center(gl);
}

static void buildUsbSection(lv_obj_t *parent) {
    lv_obj_t *card = makeCard(parent, "USB");

    s_usb_active_lbl = makeKvRow(card, "Active");
    s_usb_pref_lbl   = makeKvRow(card, "Preferred");

    // Mode buttons in a single row (3 modes).
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 44);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row, 4, LV_PART_MAIN);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(row);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, 40);
        lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
        lv_obj_add_event_cb(b, usbModeClicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(b);
        lv_label_set_text(lbl, USB_LABELS[i]);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);
        s_usb_btns[i] = b;
    }

    usbRefresh();
}

// ---- WiFi section ---------------------------------------------------------

// Modal with a textarea + lv_keyboard for editing WiFi credentials.
// Two textareas (SSID + password); the keyboard re-targets whichever
// one was last focused. Save button writes BoardSettings::setWifi AND
// triggers a reconnect. We do NOT also write WifiManager NVS namespace
// in this commit -- Phase 2 plan tracks the unification as a separate
// follow-up so this commit stays scoped.

static lv_obj_t *s_wifi_modal     = nullptr;
static lv_obj_t *s_wifi_ta_ssid   = nullptr;
static lv_obj_t *s_wifi_ta_pass   = nullptr;
static lv_obj_t *s_wifi_ssid_lbl  = nullptr;
static lv_obj_t *s_wifi_keyboard  = nullptr;
static lv_obj_t *s_wifi_scan_btn  = nullptr;
// Scan view (visible only while showing results, replaces the form):
static lv_obj_t *s_wifi_scan_panel  = nullptr;   // holds list + back btn
static lv_obj_t *s_wifi_scan_status = nullptr;
static lv_obj_t *s_wifi_scan_list   = nullptr;
static lv_timer_t *s_wifi_scan_tick = nullptr;

static void wifiSaveClicked(lv_event_t * /*e*/);
static void wifiCancelClicked(lv_event_t * /*e*/);
static void wifiScanClicked(lv_event_t * /*e*/);
static void wifiScanBackClicked(lv_event_t * /*e*/);
static void wifiTextareaFocused(lv_event_t *e) {
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    if (s_wifi_keyboard) lv_keyboard_set_textarea(s_wifi_keyboard, ta);
}

// Helpers to swap modal between credentials-form view and scan-results view.
// We keep the form widgets alive in both states (just hide/show) so the
// scan list tap can back-fill the SSID textarea cleanly.

static void wifiShowForm() {
    if (s_wifi_scan_panel) {
        lv_obj_delete(s_wifi_scan_panel);
        s_wifi_scan_panel = nullptr;
        s_wifi_scan_status = nullptr;
        s_wifi_scan_list = nullptr;
    }
    if (s_wifi_scan_tick) {
        lv_timer_delete(s_wifi_scan_tick);
        s_wifi_scan_tick = nullptr;
    }
    // Re-show the form widgets if they were hidden.
    if (s_wifi_ta_ssid)  lv_obj_clear_flag(s_wifi_ta_ssid,  LV_OBJ_FLAG_HIDDEN);
    if (s_wifi_ta_pass)  lv_obj_clear_flag(s_wifi_ta_pass,  LV_OBJ_FLAG_HIDDEN);
    if (s_wifi_scan_btn) lv_obj_clear_flag(s_wifi_scan_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_wifi_keyboard) lv_obj_clear_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
}

// Tap on a scan result -> back-fill SSID and return to form view.
static void wifiPickResult(lv_event_t *e) {
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if (s_wifi_ta_ssid && ssid) {
        lv_textarea_set_text(s_wifi_ta_ssid, ssid);
    }
    wifiShowForm();
    // Focus the password textarea so the keyboard targets it next.
    if (s_wifi_keyboard && s_wifi_ta_pass) {
        lv_keyboard_set_textarea(s_wifi_keyboard, s_wifi_ta_pass);
    }
}

static void wifiScanTick(lv_timer_t * /*t*/) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;        // -1, still scanning
    // Either completed (n >= 0) or failed (n == -2). One-shot: stop polling.
    if (s_wifi_scan_tick) {
        lv_timer_delete(s_wifi_scan_tick);
        s_wifi_scan_tick = nullptr;
    }

    if (!s_wifi_scan_status || !s_wifi_scan_list) return;

    if (n < 0) {
        lv_label_set_text(s_wifi_scan_status, "scan failed -- try again");
        lv_obj_set_style_text_color(s_wifi_scan_status, lv_color_hex(0xE63946), 0);
        return;
    }
    if (n == 0) {
        lv_label_set_text(s_wifi_scan_status, "no networks found");
        lv_obj_set_style_text_color(s_wifi_scan_status, lv_color_hex(0xE6A23C), 0);
        return;
    }

    char hdr[40];
    snprintf(hdr, sizeof(hdr), "found %d networks", n);
    lv_label_set_text(s_wifi_scan_status, hdr);
    lv_obj_set_style_text_color(s_wifi_scan_status, lv_color_hex(0x06A77D), 0);

    // Build buttons. Strdup the SSID so the user_data outlives the
    // WiFi.scanDelete() call below.
    int show = n > 16 ? 16 : n;
    for (int i = 0; i < show; i++) {
        String ssid = WiFi.SSID(i);
        int32_t rssi = WiFi.RSSI(i);
        bool open  = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        if (ssid.length() == 0) ssid = "(hidden)";

        char *ssid_owned = strdup(ssid.c_str());

        lv_obj_t *row = lv_button_create(s_wifi_scan_list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 36);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1a1f24), LV_PART_MAIN);
        lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
        lv_obj_add_event_cb(row, wifiPickResult, LV_EVENT_CLICKED, ssid_owned);

        char line[80];
        snprintf(line, sizeof(line), "%s%s   %d dBm",
                 ssid.c_str(), open ? " " : " *", (int)rssi);
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, line);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
    }

    WiFi.scanDelete();
}

static void wifiScanClicked(lv_event_t * /*e*/) {
    if (!s_wifi_modal) return;
    Safety::logf("[settings] wifi scan requested");

    // Hide form widgets while results are visible.
    if (s_wifi_ta_ssid)  lv_obj_add_flag(s_wifi_ta_ssid,  LV_OBJ_FLAG_HIDDEN);
    if (s_wifi_ta_pass)  lv_obj_add_flag(s_wifi_ta_pass,  LV_OBJ_FLAG_HIDDEN);
    if (s_wifi_scan_btn) lv_obj_add_flag(s_wifi_scan_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_wifi_keyboard) lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);

    // Build scan view inside the modal.
    s_wifi_scan_panel = lv_obj_create(s_wifi_modal);
    lv_obj_set_width(s_wifi_scan_panel, lv_pct(100));
    lv_obj_set_flex_grow(s_wifi_scan_panel, 1);
    lv_obj_set_style_bg_opa(s_wifi_scan_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_wifi_scan_panel, 0, 0);
    lv_obj_set_style_border_width(s_wifi_scan_panel, 0, 0);
    lv_obj_set_layout(s_wifi_scan_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_wifi_scan_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_wifi_scan_panel, 4, 0);

    s_wifi_scan_status = lv_label_create(s_wifi_scan_panel);
    lv_label_set_text(s_wifi_scan_status, "Scanning...");
    lv_obj_set_style_text_color(s_wifi_scan_status, lv_color_hex(0xa0a0a0), 0);
    lv_obj_set_style_text_font(s_wifi_scan_status, &lv_font_montserrat_14, 0);

    s_wifi_scan_list = lv_obj_create(s_wifi_scan_panel);
    lv_obj_set_width(s_wifi_scan_list, lv_pct(100));
    lv_obj_set_flex_grow(s_wifi_scan_list, 1);
    lv_obj_set_style_bg_opa(s_wifi_scan_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_wifi_scan_list, 4, 0);
    lv_obj_set_style_border_width(s_wifi_scan_list, 0, 0);
    lv_obj_set_layout(s_wifi_scan_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_wifi_scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_wifi_scan_list, 3, 0);
    lv_obj_set_scroll_dir(s_wifi_scan_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_wifi_scan_list, LV_SCROLLBAR_MODE_AUTO);

    // Back button so user can abort scan and return to form.
    lv_obj_t *back = lv_button_create(s_wifi_scan_panel);
    lv_obj_set_width(back, lv_pct(100));
    lv_obj_set_height(back, 40);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x394150), LV_PART_MAIN);
    lv_obj_set_style_radius(back, 6, 0);
    lv_obj_add_event_cb(back, wifiScanBackClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, "< Back to form");
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_center(bl);

    // Kick off async scan + poll.
    WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
    s_wifi_scan_tick = lv_timer_create(wifiScanTick, 500, nullptr);
}

static void wifiScanBackClicked(lv_event_t * /*e*/) {
    // Abort an in-flight scan if any (scanComplete handles the delete).
    if (WiFi.scanComplete() >= 0) WiFi.scanDelete();
    wifiShowForm();
}

static void wifiEditClicked(lv_event_t * /*e*/) {
    Safety::logf("[settings] wifiEditClicked fired (modal=%p kb=%p)",
                 (void *)s_wifi_modal, (void *)s_wifi_keyboard);
    if (s_wifi_modal) {
        Safety::logf("[settings] modal already open -- early return");
        return;
    }
    Safety::logf("[settings] building modal...");

    s_wifi_modal = lv_obj_create(lv_screen_active());
    // Sit BELOW the 24 px status bar so the network state stays visible.
    lv_obj_set_size(s_wifi_modal, 320, 480 - 24);
    lv_obj_set_pos(s_wifi_modal, 0, 24);
    lv_obj_set_style_bg_color(s_wifi_modal, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_wifi_modal, 8, LV_PART_MAIN);
    lv_obj_set_layout(s_wifi_modal, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_wifi_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_wifi_modal, 6, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(s_wifi_modal);
    lv_label_set_text(title, "WiFi");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);

    // Scan button -- swaps modal into scan-results view; tap a network
    // to back-fill the SSID textarea.
    s_wifi_scan_btn = lv_button_create(s_wifi_modal);
    lv_obj_set_width(s_wifi_scan_btn, lv_pct(100));
    lv_obj_set_height(s_wifi_scan_btn, 36);
    lv_obj_set_style_bg_color(s_wifi_scan_btn, lv_color_hex(0x394150), LV_PART_MAIN);
    lv_obj_set_style_radius(s_wifi_scan_btn, 6, 0);
    lv_obj_add_event_cb(s_wifi_scan_btn, wifiScanClicked, LV_EVENT_CLICKED, nullptr);
    {
        lv_obj_t *l = lv_label_create(s_wifi_scan_btn);
        lv_label_set_text(l, "Scan networks");
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_center(l);
    }

    s_wifi_ta_ssid = lv_textarea_create(s_wifi_modal);
    lv_obj_set_width(s_wifi_ta_ssid, lv_pct(100));
    lv_obj_set_height(s_wifi_ta_ssid, 36);
    lv_textarea_set_one_line(s_wifi_ta_ssid, true);
    lv_textarea_set_placeholder_text(s_wifi_ta_ssid, "SSID");
    lv_textarea_set_text(s_wifi_ta_ssid, BoardSettings::wifiSsid().c_str());
    lv_obj_add_event_cb(s_wifi_ta_ssid, wifiTextareaFocused, LV_EVENT_FOCUSED, nullptr);

    s_wifi_ta_pass = lv_textarea_create(s_wifi_modal);
    lv_obj_set_width(s_wifi_ta_pass, lv_pct(100));
    lv_obj_set_height(s_wifi_ta_pass, 36);
    lv_textarea_set_one_line(s_wifi_ta_pass, true);
    lv_textarea_set_placeholder_text(s_wifi_ta_pass, "password");
    lv_textarea_set_text(s_wifi_ta_pass, BoardSettings::wifiPass().c_str());
    lv_obj_add_event_cb(s_wifi_ta_pass, wifiTextareaFocused, LV_EVENT_FOCUSED, nullptr);

    lv_obj_t *btn_row = lv_obj_create(s_wifi_modal);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, 44);
    lv_obj_set_layout(btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(btn_row, 6, LV_PART_MAIN);

    lv_obj_t *cancel = lv_button_create(btn_row);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_set_height(cancel, 40);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x394150), LV_PART_MAIN);
    lv_obj_add_event_cb(cancel, wifiCancelClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_center(cl);

    lv_obj_t *save = lv_button_create(btn_row);
    lv_obj_set_flex_grow(save, 1);
    lv_obj_set_height(save, 40);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x06A77D), LV_PART_MAIN);
    lv_obj_add_event_cb(save, wifiSaveClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, "Save & Connect");
    lv_obj_set_style_text_color(sl, lv_color_white(), 0);
    lv_obj_center(sl);

    // Keyboard pinned to the bottom of the screen, above modal content.
    s_wifi_keyboard = lv_keyboard_create(lv_screen_active());
    lv_obj_set_size(s_wifi_keyboard, lv_pct(100), 200);
    lv_obj_align(s_wifi_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_wifi_keyboard, s_wifi_ta_ssid);
    Safety::logf("[settings] modal+kb built");
}

static void closeWifiModal() {
    // Kill the scan poll timer + abort any running scan first.
    if (s_wifi_scan_tick) {
        lv_timer_delete(s_wifi_scan_tick);
        s_wifi_scan_tick = nullptr;
    }
    if (WiFi.scanComplete() >= 0) WiFi.scanDelete();

    if (s_wifi_keyboard) { lv_obj_delete(s_wifi_keyboard); s_wifi_keyboard = nullptr; }
    if (s_wifi_modal)    { lv_obj_delete(s_wifi_modal);    s_wifi_modal = nullptr; }
    s_wifi_ta_ssid = s_wifi_ta_pass = nullptr;
    s_wifi_scan_btn = s_wifi_scan_panel = nullptr;
    s_wifi_scan_status = s_wifi_scan_list = nullptr;
}

static void wifiCancelClicked(lv_event_t * /*e*/) {
    closeWifiModal();
}

static void wifiSaveClicked(lv_event_t * /*e*/) {
    String ssid = lv_textarea_get_text(s_wifi_ta_ssid);
    String pass = lv_textarea_get_text(s_wifi_ta_pass);
    BoardSettings::setWifi(ssid, pass);
    if (s_wifi_ssid_lbl) lv_label_set_text(s_wifi_ssid_lbl, ssid.c_str());
    Serial.printf("[settings] saved wifi ssid=\"%s\", reconnecting\n", ssid.c_str());

    // Reconnect happens in background via WiFi.begin so the UI doesn't
    // block. The status bar's WiFi icon will turn green when STA comes up.
    WiFi.disconnect(true, false);
    delay(100);
    WiFi.begin(ssid.c_str(), pass.c_str());

    closeWifiModal();
}

static void buildWifiSection(lv_obj_t *parent) {
    lv_obj_t *card = makeCard(parent, "WiFi");

    s_wifi_ssid_lbl = makeKvRow(card, "SSID");
    String ssid = BoardSettings::wifiSsid();
    lv_label_set_text(s_wifi_ssid_lbl,
                      ssid.length() ? ssid.c_str() : "<not set>");

    lv_obj_t *btn = lv_button_create(card);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 44);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x394150), LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, wifiEditClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Edit credentials...");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
}

// ---- Port B section -------------------------------------------------------
//
// User-facing knob for the Port B mode mux. Tap a mode button to release
// whatever currently holds Port B and re-acquire it in the chosen mode,
// persisting the choice to NVS as the new "preferred" boot mode. "Force
// Release" leaves Port B in IDLE without touching the preferred-mode
// pref -- useful when a screen got stuck holding the port.

static lv_obj_t *s_portb_state_lbl = nullptr;
static lv_obj_t *s_portb_pref_lbl  = nullptr;
static lv_obj_t *s_portb_pins_lbl  = nullptr;

// Order matches the on-screen button row.
static const PortMode PORTB_MODES[5] = {
    PORT_IDLE, PORT_I2C, PORT_UART, PORT_PWM, PORT_GPIO,
};
static const char *PORTB_LABELS[5] = { "OFF", "I2C", "UART", "PWM", "GPIO" };
static lv_obj_t *s_portb_btns[5] = { nullptr };

static void portBRefresh() {
    if (!s_portb_state_lbl) return;

    PortMode cur   = PinPort::currentMode(PinPort::PORT_B);
    PortMode pref  = PinPort::preferredMode(PinPort::PORT_B);
    const char *owner = PinPort::currentOwner(PinPort::PORT_B);

    char buf[48];
    snprintf(buf, sizeof(buf), "%s%s%s",
             PinPort::modeName(cur),
             (owner && *owner) ? " / " : "",
             (owner && *owner) ? owner : "");
    lv_label_set_text(s_portb_state_lbl, buf);
    lv_obj_set_style_text_color(s_portb_state_lbl,
        lv_color_hex(cur == PORT_IDLE ? 0xa0a0a0 : 0x06A77D), 0);

    lv_label_set_text(s_portb_pref_lbl, PinPort::modeName(pref));

    snprintf(buf, sizeof(buf), "SDA/TX=%d  SCL/RX=%d",
             PinPort::tx_pin(PinPort::PORT_B),
             PinPort::rx_pin(PinPort::PORT_B));
    lv_label_set_text(s_portb_pins_lbl, buf);

    // Highlight the button matching the current mode.
    for (int i = 0; i < 5; i++) {
        if (!s_portb_btns[i]) continue;
        bool active = (PORTB_MODES[i] == cur);
        lv_obj_set_style_bg_color(s_portb_btns[i],
            lv_color_hex(active ? 0x06A77D : 0x394150), LV_PART_MAIN);
    }
}

static void portBModeClicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= 5) return;
    PortMode want = PORTB_MODES[idx];

    // Apply both runtime and preferred-on-boot. Release first because
    // the previous owner might be in a different mode (acquire would
    // reject otherwise).
    PinPort::release(PinPort::PORT_B);
    if (want != PORT_IDLE) {
        if (!PinPort::acquire(PinPort::PORT_B, want, "settings_ui")) {
            Safety::logf("[settings] portB acquire(%s) FAILED",
                         PinPort::modeName(want));
            // Fall through and still save preferred -- next boot will retry.
        }
    }
    PinPort::setPreferredMode(PinPort::PORT_B, want);
    Safety::logf("[settings] Port B -> %s (preferred saved)",
                 PinPort::modeName(want));
    portBRefresh();
}

static void portBReleaseClicked(lv_event_t * /*e*/) {
    PinPort::release(PinPort::PORT_B);
    Safety::logf("[settings] Port B force-released");
    portBRefresh();
}

static void buildPortBSection(lv_obj_t *parent) {
    lv_obj_t *card = makeCard(parent, "Port B");

    s_portb_state_lbl = makeKvRow(card, "Mode");
    s_portb_pref_lbl  = makeKvRow(card, "Preferred");
    s_portb_pins_lbl  = makeKvRow(card, "Pins");

    // Mode-switch buttons, single row with flex_grow.
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 44);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row, 4, LV_PART_MAIN);

    for (int i = 0; i < 5; i++) {
        lv_obj_t *btn = lv_button_create(row);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, 40);
        lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, portBModeClicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, PORTB_LABELS[i]);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);
        s_portb_btns[i] = btn;
    }

    // Force-release button (red).
    lv_obj_t *rel = lv_button_create(card);
    lv_obj_set_width(rel, lv_pct(100));
    lv_obj_set_height(rel, 36);
    lv_obj_set_style_bg_color(rel, lv_color_hex(0xE63946), LV_PART_MAIN);
    lv_obj_set_style_radius(rel, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(rel, portBReleaseClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *rl = lv_label_create(rel);
    lv_label_set_text(rl, "Force release (-> IDLE)");
    lv_obj_set_style_text_color(rl, lv_color_white(), 0);
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_14, 0);
    lv_obj_center(rl);

    portBRefresh();
}

// ---- About section --------------------------------------------------------

static void rebootClicked(lv_event_t * /*e*/) {
    Serial.println("[settings] user-requested reboot");
    delay(50);
    esp_restart();
}

static void buildAboutSection(lv_obj_t *parent) {
    lv_obj_t *card = makeCard(parent, "About");

    s_about_ip     = makeKvRow(card, "IP");
    s_about_uptime = makeKvRow(card, "Uptime");
    s_about_heap   = makeKvRow(card, "Heap");
    s_about_ota    = makeKvRow(card, "OTA");

    aboutTick(nullptr);   // populate once immediately

    lv_obj_t *btn = lv_button_create(card);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 44);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xE63946), LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, rebootClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Reboot");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
}

// ---- Public entry ---------------------------------------------------------

static void settingsCleanup(lv_event_t * /*e*/) {
    if (s_about_timer) { lv_timer_delete(s_about_timer); s_about_timer = nullptr; }
    if (s_wifi_scan_tick) {
        lv_timer_delete(s_wifi_scan_tick);
        s_wifi_scan_tick = nullptr;
    }
    s_about_uptime = s_about_heap = s_about_ip = s_about_ota = nullptr;
    // WiFi modal widgets if open get deleted with the panel anyway.
    s_wifi_modal = s_wifi_ta_ssid = s_wifi_ta_pass = nullptr;
    s_wifi_ssid_lbl = s_wifi_keyboard = nullptr;
    s_wifi_scan_btn = s_wifi_scan_panel = nullptr;
    s_wifi_scan_status = s_wifi_scan_list = nullptr;
    // Port B card statics.
    s_portb_state_lbl = s_portb_pref_lbl = s_portb_pins_lbl = nullptr;
    for (int i = 0; i < 5; i++) s_portb_btns[i] = nullptr;
    // USB card statics.
    s_usb_active_lbl = s_usb_pref_lbl = nullptr;
    for (int i = 0; i < 3; i++) s_usb_btns[i] = nullptr;
    s_usb_confirm_modal = nullptr;   // deleted with the panel
}

void buildSettings(lv_obj_t *panel) {
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(panel, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(panel, settingsCleanup, LV_EVENT_DELETE, nullptr);

    buildDisplaySection(panel);
    buildPortBSection(panel);
    buildUsbSection(panel);
    buildWifiSection(panel);
    buildAboutSection(panel);

    // 1 Hz refresh of the live About fields. Prior timer is killed by
    // settingsCleanup on panel destroy.
    s_about_timer = lv_timer_create(aboutTick, 1000, nullptr);
}

} // namespace screens
