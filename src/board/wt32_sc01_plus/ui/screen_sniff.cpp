// RC Sniff screen -- live frame sniffer for SBUS / iBus / PPM.
//
// Wraps RCSniffer (the same module the web /api/rc/* endpoints call).
// Auto-detect runs through all three protocols for ~500 ms each on
// Port B (UART for SBUS/iBus, GPIO for PPM) and locks onto whichever
// produces a valid frame. Once locked the channel bars + frame rate
// + failsafe flags refresh at 4 Hz.
//
// Layout (320x408 panel, scrollable column):
//   Status card  : Protocol / State / Rate / Frames / CRC / Failsafe / LostFrame
//   Action row   : Auto-detect / Stop
//   Channels card: CH1..CH8 with horizontal bar + microsecond value

#include "screens.h"

#include <Arduino.h>

#include "rc_sniffer/rc_sniffer.h"
#include "core/pin_port.h"
#include "safety.h"

namespace screens {

// ---- Live widgets ---------------------------------------------------------

static lv_obj_t *s_proto_lbl   = nullptr;
static lv_obj_t *s_state_lbl   = nullptr;
static lv_obj_t *s_rate_lbl    = nullptr;
static lv_obj_t *s_frames_lbl  = nullptr;
static lv_obj_t *s_crc_lbl     = nullptr;
static lv_obj_t *s_flags_lbl   = nullptr;
static lv_obj_t *s_detect_btn  = nullptr;
static lv_obj_t *s_detect_lbl  = nullptr;
static lv_obj_t *s_stop_btn    = nullptr;
static lv_obj_t *s_ch_bars[8]  = {nullptr};
static lv_obj_t *s_ch_vals[8]  = {nullptr};
static lv_timer_t *s_tick      = nullptr;
static bool       s_detect_failed = false;   // set by detectClicked, cleared on next start

// Auto-detect state machine (lives below detectAdvance, declared up
// here so sniffCleanup can null them on screen teardown).
static lv_timer_t *s_detect_timer = nullptr;
static int         s_detect_phase = -1;     // -1 idle, 0..2 = which proto
static uint32_t    s_detect_start_frames = 0;

// Channel ranges differ per protocol but they all settle close to
// 988..2012 us (~SBUS spec); use that as the bar normaliser.
static constexpr int CH_MIN_US = 988;
static constexpr int CH_MAX_US = 2012;

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

static void makeChannelRow(lv_obj_t *parent, int idx) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 20);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 6, LV_PART_MAIN);

    char lbl[8]; snprintf(lbl, sizeof(lbl), "CH%d", idx + 1);
    lv_obj_t *k = lv_label_create(row);
    lv_obj_set_style_text_color(k, lv_color_hex(0xa0a0a0), 0);
    lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);
    lv_obj_set_width(k, 36);
    lv_label_set_text(k, lbl);

    lv_obj_t *bar = lv_bar_create(row);
    lv_obj_set_flex_grow(bar, 1);
    lv_obj_set_height(bar, 12);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x394150), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x2E86AB), LV_PART_INDICATOR);
    s_ch_bars[idx] = bar;

    lv_obj_t *v = lv_label_create(row);
    lv_obj_set_style_text_color(v, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_set_width(v, 50);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(v, "...");
    s_ch_vals[idx] = v;
}

// Fires when the panel is deleted on Back. Kills only OUR tick + the
// in-flight auto-detect timer (if any), nulls our static widget pointers.
// LVGL's internal timers MUST not be touched.
static void sniffCleanup(lv_event_t * /*e*/) {
    if (s_tick) { lv_timer_delete(s_tick); s_tick = nullptr; }
    if (s_detect_timer) {
        lv_timer_delete(s_detect_timer);
        s_detect_timer = nullptr;
    }
    s_detect_phase = -1;
    s_proto_lbl = s_state_lbl = s_rate_lbl = nullptr;
    s_frames_lbl = s_crc_lbl = s_flags_lbl = nullptr;
    s_detect_btn = s_detect_lbl = s_stop_btn = nullptr;
    for (int i = 0; i < 8; i++) { s_ch_bars[i] = s_ch_vals[i] = nullptr; }
}

// ---- Live refresh ---------------------------------------------------------

static void clearChannels() {
    for (int i = 0; i < 8; i++) {
        if (s_ch_bars[i]) lv_bar_set_value(s_ch_bars[i], 0, LV_ANIM_OFF);
        if (s_ch_vals[i]) lv_label_set_text(s_ch_vals[i], "...");
    }
}

static void sniffTick(lv_timer_t * /*t*/) {
    if (!s_state_lbl) return;

    bool running = RCSniffer::isRunning();
    if (s_stop_btn) {
        if (running) lv_obj_clear_flag(s_stop_btn, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(s_stop_btn, LV_OBJ_FLAG_HIDDEN);
    }
    // Auto-detect button gets a "Detecting..." label override during the
    // synchronous autoDetect call -- we restore it here on every tick so
    // a stale label can't get stuck.
    if (s_detect_lbl) {
        lv_label_set_text(s_detect_lbl, running ? "Re-detect" : "Auto-detect");
    }

    if (!running) {
        lv_label_set_text(s_proto_lbl, "...");
        if (s_detect_failed) {
            lv_label_set_text(s_state_lbl, "no signal -- check wiring");
            lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0xE6A23C), 0);
        } else {
            lv_label_set_text(s_state_lbl, "service stopped");
            lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0xa0a0a0), 0);
        }
        lv_label_set_text(s_rate_lbl,   "...");
        lv_label_set_text(s_frames_lbl, "...");
        lv_label_set_text(s_crc_lbl,    "...");
        lv_label_set_text(s_flags_lbl,  "...");
        clearChannels();
        return;
    }

    const auto &st = RCSniffer::state();
    char buf[40];

    lv_label_set_text(s_proto_lbl, RCSniffer::protoName(st.proto));

    if (st.connected) {
        snprintf(buf, sizeof(buf), "CONNECTED (%u ch)", (unsigned)st.channelCount);
        lv_label_set_text(s_state_lbl, buf);
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0x06A77D), 0);
    } else {
        lv_label_set_text(s_state_lbl, "no signal");
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0xE6A23C), 0);
    }

    snprintf(buf, sizeof(buf), "%lu Hz", (unsigned long)st.frameRateHz);
    lv_label_set_text(s_rate_lbl, buf);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)st.frameCount);
    lv_label_set_text(s_frames_lbl, buf);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)st.crcErrors);
    lv_label_set_text(s_crc_lbl, buf);

    // SBUS-only flags. iBus/PPM leave them false, which renders as "ok".
    if (st.failsafe) {
        lv_label_set_text(s_flags_lbl, "FAILSAFE");
        lv_obj_set_style_text_color(s_flags_lbl, lv_color_hex(0xE63946), 0);
    } else if (st.lostFrame) {
        lv_label_set_text(s_flags_lbl, "lost frame");
        lv_obj_set_style_text_color(s_flags_lbl, lv_color_hex(0xE6A23C), 0);
    } else {
        lv_label_set_text(s_flags_lbl, "ok");
        lv_obj_set_style_text_color(s_flags_lbl, lv_color_hex(0x06A77D), 0);
    }

    int n = st.channelCount > 8 ? 8 : st.channelCount;
    for (int i = 0; i < n; i++) {
        int us = st.channels[i];
        int pct = ((us - CH_MIN_US) * 100) / (CH_MAX_US - CH_MIN_US);
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        lv_bar_set_value(s_ch_bars[i], pct, LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "%d", us);
        lv_label_set_text(s_ch_vals[i], buf);
    }
    for (int i = n; i < 8; i++) {
        if (s_ch_bars[i]) lv_bar_set_value(s_ch_bars[i], 0, LV_ANIM_OFF);
        if (s_ch_vals[i]) lv_label_set_text(s_ch_vals[i], "...");
    }
}

// ---- Event handlers -------------------------------------------------------

// RCSniffer::autoDetect() is a 1500 ms busy-wait blocker (3 protocols x
// 500 ms each). Calling it inline from the click handler stalls
// lv_timer_handler -- the status bar timer freezes, input feels dead,
// and the AsyncTCP task hits its watchdog if a request arrives mid-detect.
//
// Replace with a state machine: each phase fires synchronously (just
// stop+start the next protocol candidate, ~few ms), then schedules a
// one-shot lv_timer 500 ms later to evaluate frame count and advance.
// LVGL + main loop run normally between phases. RCSniffer::loop() is
// already pumped from main_sc01_plus.cpp so frame parsing keeps going.
//
// The s_detect_* statics live up at the file scope so sniffCleanup
// (which lives above this block) can null them.

static const RCProto DETECT_ORDER[3] = {
    RC_PROTO_SBUS, RC_PROTO_IBUS, RC_PROTO_PPM
};

static void detectFinish(bool ok) {
    s_detect_phase = -1;
    s_detect_failed = !ok;
    Safety::logf("[sniff] auto-detect -> proto=%s",
        RCSniffer::protoName(RCSniffer::state().proto));
    sniffTick(nullptr);
}

static void detectAdvance(lv_timer_t * /*t*/) {
    s_detect_timer = nullptr;   // one-shot fired; clear so cleanup is safe

    // Did the previous phase pick up any frames?
    if (s_detect_phase >= 0) {
        if (RCSniffer::state().frameCount > s_detect_start_frames) {
            // Locked on. Stop probing, leave the running session as-is.
            detectFinish(true);
            return;
        }
    }

    s_detect_phase++;
    if (s_detect_phase >= (int)(sizeof(DETECT_ORDER) / sizeof(DETECT_ORDER[0]))) {
        // Exhausted all candidates -- nothing on the wire.
        RCSniffer::stop();
        detectFinish(false);
        return;
    }

    // Switch to the next candidate. start() reconfigures Serial1, fast.
    if (RCSniffer::isRunning()) RCSniffer::stop();
    RCSniffer::start(DETECT_ORDER[s_detect_phase]);
    s_detect_start_frames = RCSniffer::state().frameCount;
    Safety::logf("[sniff] phase %d: trying %s",
                 s_detect_phase, RCSniffer::protoName(DETECT_ORDER[s_detect_phase]));

    // Schedule the next evaluation 500 ms from now.
    s_detect_timer = lv_timer_create(detectAdvance, 500, nullptr);
    lv_timer_set_repeat_count(s_detect_timer, 1);
}

static void detectClicked(lv_event_t * /*e*/) {
    if (s_detect_phase >= 0) return;   // detection already running, ignore re-tap

    if (RCSniffer::isRunning()) RCSniffer::stop();
    if (s_detect_lbl) lv_label_set_text(s_detect_lbl, "Detecting...");
    Safety::logf("[sniff] auto-detect start");
    s_detect_failed = false;

    s_detect_phase = -1;   // detectAdvance will increment to 0 first
    detectAdvance(nullptr);
}

static void stopClicked(lv_event_t * /*e*/) {
    RCSniffer::stop();
    s_detect_failed = false;
    Safety::logf("[sniff] stopped");
    sniffTick(nullptr);
}

// ---- Section builders -----------------------------------------------------

static void buildStatusCard(lv_obj_t *parent) {
    lv_obj_t *card = makeCard(parent, "Frame");
    s_proto_lbl  = makeKvRow(card, "Protocol");
    s_state_lbl  = makeKvRow(card, "State");
    s_rate_lbl   = makeKvRow(card, "Rate");
    s_frames_lbl = makeKvRow(card, "Frames");
    s_crc_lbl    = makeKvRow(card, "CRC errors");
    s_flags_lbl  = makeKvRow(card, "Status");
}

static void buildActionRow(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 50);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row, 6, LV_PART_MAIN);

    s_detect_btn = lv_button_create(row);
    lv_obj_set_flex_grow(s_detect_btn, 1);
    lv_obj_set_height(s_detect_btn, 50);
    lv_obj_set_style_radius(s_detect_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_detect_btn, lv_color_hex(0x06A77D), LV_PART_MAIN);
    lv_obj_add_event_cb(s_detect_btn, detectClicked, LV_EVENT_CLICKED, nullptr);
    s_detect_lbl = lv_label_create(s_detect_btn);
    lv_label_set_text(s_detect_lbl, "Auto-detect");
    lv_obj_set_style_text_color(s_detect_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_detect_lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(s_detect_lbl);

    s_stop_btn = lv_button_create(row);
    lv_obj_set_flex_grow(s_stop_btn, 1);
    lv_obj_set_height(s_stop_btn, 50);
    lv_obj_set_style_radius(s_stop_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_stop_btn, lv_color_hex(0xE63946), LV_PART_MAIN);
    lv_obj_add_event_cb(s_stop_btn, stopClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *sl = lv_label_create(s_stop_btn);
    lv_label_set_text(sl, "Stop");
    lv_obj_set_style_text_color(sl, lv_color_white(), 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_18, 0);
    lv_obj_center(sl);
    lv_obj_add_flag(s_stop_btn, LV_OBJ_FLAG_HIDDEN);   // shown only when running
}

static void buildChannelsCard(lv_obj_t *parent) {
    lv_obj_t *card = makeCard(parent, "Channels (CH1-CH8)");
    for (int i = 0; i < 8; i++) makeChannelRow(card, i);
}

// ---- Public entry ---------------------------------------------------------

void buildSniff(lv_obj_t *panel) {
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(panel, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(panel, sniffCleanup, LV_EVENT_DELETE, nullptr);

    buildStatusCard(panel);
    buildActionRow(panel);
    buildChannelsCard(panel);

    sniffTick(nullptr);

    // Prior tick is killed by sniffCleanup when the panel is destroyed.
    s_tick = lv_timer_create(sniffTick, 250, nullptr);   // 4 Hz
}

} // namespace screens
