// Settings screen -- configuration UI replacing the Serial CLI.
//
// Sections:
//   Display  : rotation 0/1/2/3, brightness slider, touch recalibrate
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

namespace screens {

// ---- About section live-refresh -------------------------------------------

static lv_obj_t *s_about_uptime = nullptr;
static lv_obj_t *s_about_heap   = nullptr;
static lv_obj_t *s_about_ip     = nullptr;
static lv_obj_t *s_about_ota    = nullptr;
static lv_timer_t *s_about_timer = nullptr;

static void aboutTick(lv_timer_t * /*t*/) {
    if (!s_about_uptime) return;   // section was rebuilt / left
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
    lv_slider_set_value(sl, 255, LV_ANIM_OFF);  // we don't store this in NVS yet
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

static void wifiSaveClicked(lv_event_t * /*e*/);
static void wifiCancelClicked(lv_event_t * /*e*/);
static void wifiTextareaFocused(lv_event_t *e) {
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    if (s_wifi_keyboard) lv_keyboard_set_textarea(s_wifi_keyboard, ta);
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
    if (s_wifi_keyboard) { lv_obj_delete(s_wifi_keyboard); s_wifi_keyboard = nullptr; }
    if (s_wifi_modal)    { lv_obj_delete(s_wifi_modal);    s_wifi_modal = nullptr; }
    s_wifi_ta_ssid = s_wifi_ta_pass = nullptr;
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

void buildSettings(lv_obj_t *panel) {
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(panel, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);

    buildDisplaySection(panel);
    buildWifiSection(panel);
    buildAboutSection(panel);

    // 1 Hz refresh of the live About fields. Cancel any prior timer
    // (if Settings was opened+closed+opened again, we'd leak otherwise).
    if (s_about_timer) lv_timer_delete(s_about_timer);
    s_about_timer = lv_timer_create(aboutTick, 1000, nullptr);
}

} // namespace screens
