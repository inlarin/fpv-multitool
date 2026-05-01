// Motor screen -- DShot ESC bench tester (SAFETY-CRITICAL).
//
// Drives the same WebState::motor request flags that MotorDispatch::pump()
// reads from the main loop. The pump owns DShot::init/arm/sendThrottle
// and the disarm sequence, so this screen never touches the RMT
// peripheral directly.
//
// Layout (320x408 panel, scrollable column):
//   Status card  : State (ARMED/DISARMED) / DShot speed / Throttle 0/cap
//   Hero card    : big throttle readout + slider 0..MAX_THROTTLE
//   Action row   : ARM (green) -> DISARM (red) toggle, full width
//                  Beep (gray, only visible when armed)
//
// Safety choices:
//  * Slider hard-capped at 200 / 2000 (10%) regardless of WebState
//    maxThrottle. The on-screen widget will not let a user accidentally
//    spin a propeller at full RPM. Higher is reachable via the web UI.
//  * Throttle resets to 0 on screen entry.
//  * Tile back-button keeps motor armed (ESC keeps last DShot value)
//    -- user must explicitly DISARM. This matches the web UI's behaviour
//    so the two surfaces don't disagree.

#include "screens.h"

#include <Arduino.h>

#include "core/pin_port.h"
#include "web/web_state.h"
#include "port_modal.h"
#include "safety.h"

namespace screens {

// Conservative LVGL-side throttle cap. Web UI can still go to 2000.
static constexpr int LVGL_MAX_THROTTLE = 200;

// ---- Live widgets ---------------------------------------------------------

static lv_obj_t *s_state_lbl   = nullptr;
static lv_obj_t *s_speed_lbl   = nullptr;
static lv_obj_t *s_throttle_lbl = nullptr;
static lv_obj_t *s_hero_lbl    = nullptr;
static lv_obj_t *s_slider      = nullptr;
static lv_obj_t *s_arm_btn     = nullptr;
static lv_obj_t *s_arm_lbl     = nullptr;
static lv_obj_t *s_beep_btn    = nullptr;
static lv_timer_t *s_tick      = nullptr;

// ---- Helpers --------------------------------------------------------------

static void applyThrottle(int t) {
    if (t < 0)                  t = 0;
    if (t > LVGL_MAX_THROTTLE)  t = LVGL_MAX_THROTTLE;
    {
        WebState::Lock lk;
        WebState::motor.throttle = t;
    }
    if (s_hero_lbl) {
        char buf[16]; snprintf(buf, sizeof(buf), "%d", t);
        lv_label_set_text(s_hero_lbl, buf);
    }
}

// ---- Card builder ---------------------------------------------------------

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

// Fires when the panel is deleted on Back. Kills only OUR tick + nulls
// our static widget pointers. LVGL's internal timers MUST not be touched.
static void motorCleanup(lv_event_t * /*e*/) {
    if (s_tick) { lv_timer_delete(s_tick); s_tick = nullptr; }
    PortModal::close();
    s_state_lbl = s_speed_lbl = s_throttle_lbl = nullptr;
    s_hero_lbl = s_slider = nullptr;
    s_arm_btn = s_arm_lbl = s_beep_btn = nullptr;
}

// ---- Live refresh ---------------------------------------------------------

static void motorTick(lv_timer_t * /*t*/) {
    if (!s_state_lbl) return;

    bool armed; int throttle, dshotSpeed;
    {
        WebState::Lock lk;
        armed      = WebState::motor.armed;
        throttle   = WebState::motor.throttle;
        dshotSpeed = WebState::motor.dshotSpeed;
    }

    if (armed) {
        lv_label_set_text(s_state_lbl, "ARMED");
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0xE63946), 0);
        lv_label_set_text(s_arm_lbl, "DISARM");
        lv_obj_set_style_bg_color(s_arm_btn, lv_color_hex(0xE63946), LV_PART_MAIN);
        lv_obj_clear_flag(s_beep_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_state_lbl, "DISARMED");
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0xa0a0a0), 0);
        lv_label_set_text(s_arm_lbl, "ARM");
        lv_obj_set_style_bg_color(s_arm_btn, lv_color_hex(0x06A77D), LV_PART_MAIN);
        lv_obj_add_flag(s_beep_btn, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[24];
    snprintf(buf, sizeof(buf), "DShot %d", dshotSpeed);
    lv_label_set_text(s_speed_lbl, buf);
    snprintf(buf, sizeof(buf), "%d / %d", throttle, LVGL_MAX_THROTTLE);
    lv_label_set_text(s_throttle_lbl, buf);

    // Keep the hero + slider in sync with WebState (web UI may move it too).
    if (s_hero_lbl) {
        snprintf(buf, sizeof(buf), "%d", throttle);
        lv_label_set_text(s_hero_lbl, buf);
    }
    int slider_v = throttle;
    if (slider_v > LVGL_MAX_THROTTLE) slider_v = LVGL_MAX_THROTTLE;
    if (s_slider && lv_slider_get_value(s_slider) != slider_v) {
        lv_slider_set_value(s_slider, slider_v, LV_ANIM_OFF);
    }
}

// ---- Event handlers -------------------------------------------------------

static void sliderChanged(lv_event_t *e) {
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    applyThrottle(lv_slider_get_value(sl));
}

// Actual arm-request -- runs once Port B is confirmed in PWM mode
// (synchronously if already PWM, or after the user accepts the
// PortModal switch prompt).
static void motorArmImpl() {
    WebState::Lock lk;
    WebState::motor.throttle = 0;     // safety: ESC arm sequence wants 0
    WebState::motor.armRequest = true;
    Safety::logf("[motor] ARM requested (dshot=%d)", WebState::motor.dshotSpeed);
}

static void armToggleClicked(lv_event_t * /*e*/) {
    bool armed;
    {
        WebState::Lock lk;
        armed = WebState::motor.armed;
    }
    if (armed) {
        // DISARM: set request, MotorDispatch sends 0 throttle and stops DShot.
        // Reset throttle to 0 in our snapshot so the slider doesn't jump back
        // to a non-zero value on the next arm.
        {
            WebState::Lock lk;
            WebState::motor.disarmRequest = true;
            WebState::motor.throttle = 0;
        }
        Safety::logf("[motor] DISARM requested");
        if (s_slider) lv_slider_set_value(s_slider, 0, LV_ANIM_ON);
        if (s_hero_lbl) lv_label_set_text(s_hero_lbl, "0");
        motorTick(nullptr);
        return;
    }
    // Arm path: ensure PWM via the shared modal, then run motorArmImpl.
    if (s_slider) lv_slider_set_value(s_slider, 0, LV_ANIM_ON);
    if (s_hero_lbl) lv_label_set_text(s_hero_lbl, "0");
    PortModal::ensureMode(PORT_PWM, "motor", motorArmImpl);
    motorTick(nullptr);
}

static void beepClicked(lv_event_t * /*e*/) {
    WebState::Lock lk;
    if (!WebState::motor.armed) return;
    WebState::motor.beepRequest = true;
    WebState::motor.beepCmd = 1;   // BEACON1, audible without spin
}

// ---- Public entry ---------------------------------------------------------

void buildMotor(lv_obj_t *panel) {
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(panel, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(panel, motorCleanup, LV_EVENT_DELETE, nullptr);

    // Reset throttle on every entry. Doesn't disarm the ESC -- the user
    // can come back to a still-armed motor at zero throttle.
    {
        WebState::Lock lk;
        WebState::motor.throttle = 0;
    }

    // ---- Status card ----
    lv_obj_t *status = makeCard(panel, "Status");
    s_state_lbl    = makeKvRow(status, "State");
    s_speed_lbl    = makeKvRow(status, "DShot");
    s_throttle_lbl = makeKvRow(status, "Throttle");

    // ---- Hero throttle + slider ----
    lv_obj_t *hero_card = makeCard(panel, nullptr);
    s_hero_lbl = lv_label_create(hero_card);
    lv_obj_set_width(s_hero_lbl, lv_pct(100));
    lv_obj_set_style_text_color(s_hero_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_hero_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(s_hero_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_hero_lbl, "0");

    s_slider = lv_slider_create(hero_card);
    lv_obj_set_width(s_slider, lv_pct(100));
    lv_obj_set_height(s_slider, 36);
    lv_slider_set_range(s_slider, 0, LVGL_MAX_THROTTLE);
    lv_slider_set_value(s_slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_pad_all(s_slider, 12, LV_PART_KNOB);
    lv_obj_add_event_cb(s_slider, sliderChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    // ---- ARM/DISARM toggle ----
    s_arm_btn = lv_button_create(panel);
    lv_obj_set_width(s_arm_btn, lv_pct(100));
    lv_obj_set_height(s_arm_btn, 64);
    lv_obj_set_style_radius(s_arm_btn, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(s_arm_btn, armToggleClicked, LV_EVENT_CLICKED, nullptr);
    s_arm_lbl = lv_label_create(s_arm_btn);
    lv_label_set_text(s_arm_lbl, "ARM");
    lv_obj_set_style_text_color(s_arm_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_arm_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(s_arm_lbl);

    // ---- Beep (only when armed) ----
    s_beep_btn = lv_button_create(panel);
    lv_obj_set_width(s_beep_btn, lv_pct(100));
    lv_obj_set_height(s_beep_btn, 44);
    lv_obj_set_style_radius(s_beep_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_beep_btn, lv_color_hex(0x394150), LV_PART_MAIN);
    lv_obj_add_event_cb(s_beep_btn, beepClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(s_beep_btn);
    lv_label_set_text(bl, "Beep");
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_18, 0);
    lv_obj_center(bl);
    lv_obj_add_flag(s_beep_btn, LV_OBJ_FLAG_HIDDEN);

    motorTick(nullptr);
    // Prior tick is killed by motorCleanup when the panel is destroyed.
    s_tick = lv_timer_create(motorTick, 250, nullptr);   // 4 Hz
}

} // namespace screens
