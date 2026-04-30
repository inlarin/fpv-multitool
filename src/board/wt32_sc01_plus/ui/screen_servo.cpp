// Servo screen -- bench tester for a single PWM servo.
//
// Layout (inside the 320x416 section panel):
//   Hero pulse readout (Montserrat 28, top)
//   Big slider 500..2500 us
//   Step buttons row: -50 / -10 / +10 / +50
//   Frequency selector: 50 Hz / 330 Hz
//   Start/Stop master toggle
//
// Drives ServoPWM (LEDC under the hood) via WebState::servo. The state
// struct is the same one the web UI talks to, so changes from either
// surface stay in sync.

#include "screens.h"

#include <Arduino.h>

#include "pin_config.h"
#include "servo/servo_pwm.h"
#include "web/web_state.h"
#include "safety.h"

namespace screens {

static lv_obj_t *s_pulse_lbl = nullptr;
static lv_obj_t *s_slider    = nullptr;
static lv_obj_t *s_freq_btns[2] = {nullptr, nullptr};   // 50, 330
static lv_obj_t *s_start_btn = nullptr;
static lv_obj_t *s_start_lbl = nullptr;
static lv_timer_t *s_tick    = nullptr;

static const int FREQS[2] = { 50, 330 };

// ---- helpers ---------------------------------------------------------------

static void applyPulse(int us) {
    if (us < 500)  us = 500;
    if (us > 2500) us = 2500;
    {
        WebState::Lock lk;
        WebState::servo.pulseUs = us;
    }
    if (ServoPWM::isActive()) {
        ServoPWM::setPulse(us);
    }
    if (s_pulse_lbl) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d us", us);
        lv_label_set_text(s_pulse_lbl, buf);
    }
}

static void refreshFreqButtons() {
    int cur;
    {
        WebState::Lock lk;
        cur = WebState::servo.freq;
    }
    for (int i = 0; i < 2; i++) {
        bool active = (FREQS[i] == cur);
        lv_obj_set_style_bg_color(s_freq_btns[i],
            lv_color_hex(active ? 0x06A77D : 0x394150), LV_PART_MAIN);
    }
}

static void refreshStartButton() {
    bool active;
    {
        WebState::Lock lk;
        active = WebState::servo.active;
    }
    lv_obj_set_style_bg_color(s_start_btn,
        lv_color_hex(active ? 0xE63946 : 0x06A77D), LV_PART_MAIN);
    lv_label_set_text(s_start_lbl, active ? "Stop" : "Start");
}

// ---- event handlers --------------------------------------------------------

static void sliderChanged(lv_event_t *e) {
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    applyPulse(lv_slider_get_value(sl));
}

static void stepClicked(lv_event_t *e) {
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    int cur;
    {
        WebState::Lock lk;
        cur = WebState::servo.pulseUs;
    }
    int nv = cur + delta;
    if (nv < 500)  nv = 500;
    if (nv > 2500) nv = 2500;
    if (s_slider) lv_slider_set_value(s_slider, nv, LV_ANIM_ON);
    applyPulse(nv);
}

static void freqClicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int hz  = FREQS[idx];
    {
        WebState::Lock lk;
        WebState::servo.freq = hz;
    }
    if (ServoPWM::isActive()) ServoPWM::setFrequency(hz);
    refreshFreqButtons();
}

static void startClicked(lv_event_t * /*e*/) {
    bool active = ServoPWM::isActive();
    if (active) {
        ServoPWM::stop();
        {
            WebState::Lock lk;
            WebState::servo.active = false;
        }
        Safety::logf("[servo] STOP");
    } else {
        int hz, us;
        {
            WebState::Lock lk;
            hz = WebState::servo.freq;
            us = WebState::servo.pulseUs;
        }
        ServoPWM::start(SIGNAL_OUT, hz);
        ServoPWM::setPulse(us);
        {
            WebState::Lock lk;
            WebState::servo.active = true;
        }
        Safety::logf("[servo] START hz=%d pulse=%d", hz, us);
    }
    refreshStartButton();
}

// 4 Hz refresh (live readout follows web-side changes too, e.g. someone
// moving the slider in the browser at the same time).
static void servoTick(lv_timer_t * /*t*/) {
    if (!s_pulse_lbl || !s_slider) return;
    int us;
    {
        WebState::Lock lk;
        us = WebState::servo.pulseUs;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d us", us);
    lv_label_set_text(s_pulse_lbl, buf);
    if (us != lv_slider_get_value(s_slider)) {
        lv_slider_set_value(s_slider, us, LV_ANIM_OFF);
    }
}

// ---- entry -----------------------------------------------------------------

void buildServo(lv_obj_t *panel) {
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(panel, 12, LV_PART_MAIN);

    // ---- Hero pulse readout ----
    s_pulse_lbl = lv_label_create(panel);
    lv_obj_set_style_text_color(s_pulse_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_pulse_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_width(s_pulse_lbl, lv_pct(100));
    lv_obj_set_style_text_align(s_pulse_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_pulse_lbl, "1500 us");

    // ---- Slider ----
    s_slider = lv_slider_create(panel);
    lv_obj_set_width(s_slider, lv_pct(100));
    lv_obj_set_height(s_slider, 36);
    lv_slider_set_range(s_slider, 500, 2500);
    {
        WebState::Lock lk;
        lv_slider_set_value(s_slider, WebState::servo.pulseUs, LV_ANIM_OFF);
    }
    lv_obj_add_event_cb(s_slider, sliderChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    // Bigger knob for finger taps.
    lv_obj_set_style_pad_all(s_slider, 12, LV_PART_KNOB);

    // ---- Step row: -50 / -10 / +10 / +50 ----
    lv_obj_t *step_row = lv_obj_create(panel);
    lv_obj_remove_style_all(step_row);
    lv_obj_set_width(step_row, lv_pct(100));
    lv_obj_set_height(step_row, 50);
    lv_obj_set_layout(step_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(step_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(step_row, 6, LV_PART_MAIN);
    lv_obj_set_flex_align(step_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const struct { const char *lbl; int delta; } STEPS[] = {
        { "-50", -50 }, { "-10", -10 }, { "+10", +10 }, { "+50", +50 },
    };
    for (auto &s : STEPS) {
        lv_obj_t *b = lv_button_create(step_row);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, 50);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x394150), LV_PART_MAIN);
        lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
        lv_obj_add_event_cb(b, stepClicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)s.delta);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, s.lbl);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
        lv_obj_center(l);
    }

    // ---- Frequency selector ----
    lv_obj_t *freq_row = lv_obj_create(panel);
    lv_obj_remove_style_all(freq_row);
    lv_obj_set_width(freq_row, lv_pct(100));
    lv_obj_set_height(freq_row, 50);
    lv_obj_set_layout(freq_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(freq_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(freq_row, 6, LV_PART_MAIN);
    static const char *FREQ_LBL[2] = { "50 Hz", "330 Hz" };
    for (int i = 0; i < 2; i++) {
        s_freq_btns[i] = lv_button_create(freq_row);
        lv_obj_set_flex_grow(s_freq_btns[i], 1);
        lv_obj_set_height(s_freq_btns[i], 50);
        lv_obj_set_style_radius(s_freq_btns[i], 8, LV_PART_MAIN);
        lv_obj_add_event_cb(s_freq_btns[i], freqClicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(s_freq_btns[i]);
        lv_label_set_text(l, FREQ_LBL[i]);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
        lv_obj_center(l);
    }
    refreshFreqButtons();

    // ---- Start/Stop ----
    s_start_btn = lv_button_create(panel);
    lv_obj_set_width(s_start_btn, lv_pct(100));
    lv_obj_set_height(s_start_btn, 56);
    lv_obj_set_style_radius(s_start_btn, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(s_start_btn, startClicked, LV_EVENT_CLICKED, nullptr);
    s_start_lbl = lv_label_create(s_start_btn);
    lv_obj_set_style_text_color(s_start_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_start_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(s_start_lbl);
    refreshStartButton();

    // 4 Hz tick to follow external state changes (web UI moving the slider).
    if (s_tick) lv_timer_delete(s_tick);
    s_tick = lv_timer_create(servoTick, 250, nullptr);
}

} // namespace screens
