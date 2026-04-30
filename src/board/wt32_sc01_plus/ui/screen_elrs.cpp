// ELRS RX screen -- live link monitor for a connected CRSF receiver.
//
// Layout (inside the 320x408 section panel, scrollable column):
//   Status card     : connect state + RSSI / LQ / SNR / frames / RF mode
//   Channels card   : CH1..CH8 with horizontal bar + us value (live)
//   Action row      : Start/Stop CRSF + Bind (only when running)
//
// Service control mirrors /api/crsf/{start,stop} on the web side, including
// the Port B PinPort acquire/release dance so a UART-mode acquire doesn't
// collide with the battery (I2C) or servo (PWM) screens.
//
// CRSFService::loop() is already pumped from WebServer::loop() once the
// service is running, so we don't tick the parser here -- only the UI
// label refresh runs on a 4 Hz lv_timer.

#include "screens.h"

#include <Arduino.h>

#include "core/pin_port.h"
#include "crsf/crsf_service.h"
#include "crsf/crsf_config.h"
#include "web/web_state.h"
#include "safety.h"

namespace screens {

// ---- Live widgets ---------------------------------------------------------

static lv_obj_t *s_state_lbl   = nullptr;
static lv_obj_t *s_rssi_lbl    = nullptr;
static lv_obj_t *s_lq_lbl      = nullptr;
static lv_obj_t *s_snr_lbl     = nullptr;
static lv_obj_t *s_frames_lbl  = nullptr;
static lv_obj_t *s_rf_lbl      = nullptr;
static lv_obj_t *s_ch_bars[8]  = {nullptr};
static lv_obj_t *s_ch_vals[8]  = {nullptr};
static lv_obj_t *s_start_btn   = nullptr;
static lv_obj_t *s_start_lbl   = nullptr;
static lv_obj_t *s_bind_btn    = nullptr;
static lv_timer_t *s_tick      = nullptr;

// CRSF channel value (172..1811) -> servo us (988..2012). Matches the
// Crossfire spec: 992 -> 1500, slope 5/8.
static int crsfToUs(int crsf) {
    if (crsf < 172)  crsf = 172;
    if (crsf > 1811) crsf = 1811;
    return (crsf - 992) * 5 / 8 + 1500;
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

    lv_obj_t *hdr = lv_label_create(card);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x2E86AB), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_18, 0);
    lv_label_set_text(hdr, title);
    return card;
}

// One "label : value" row. Returns the value label so the tick can update it.
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

// One "CHn  ============   1500" row inside the channels card.
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

    char lbl[8];
    snprintf(lbl, sizeof(lbl), "CH%d", idx + 1);
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

// Fires when the panel is deleted on Back. Kills only OUR tick + nulls
// our static widget pointers. LVGL's internal timers MUST not be touched.
static void elrsCleanup(lv_event_t * /*e*/) {
    if (s_tick) { lv_timer_delete(s_tick); s_tick = nullptr; }
    s_state_lbl = s_rssi_lbl = s_lq_lbl = s_snr_lbl = nullptr;
    s_frames_lbl = s_rf_lbl = nullptr;
    s_start_btn = s_start_lbl = s_bind_btn = nullptr;
    for (int i = 0; i < 8; i++) { s_ch_bars[i] = s_ch_vals[i] = nullptr; }
}

// ---- Live refresh ---------------------------------------------------------

static void elrsTick(lv_timer_t * /*t*/) {
    if (!s_state_lbl) return;   // panel torn down

    bool running = CRSFService::isRunning();
    if (s_start_lbl) {
        lv_label_set_text(s_start_lbl, running ? "Stop CRSF" : "Start CRSF");
        lv_obj_set_style_bg_color(s_start_btn,
            lv_color_hex(running ? 0xE63946 : 0x06A77D), LV_PART_MAIN);
    }
    if (s_bind_btn) {
        if (running) lv_obj_clear_flag(s_bind_btn, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(s_bind_btn, LV_OBJ_FLAG_HIDDEN);
    }

    if (!running) {
        lv_label_set_text(s_state_lbl, "service stopped");
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0xa0a0a0), 0);
        lv_label_set_text(s_rssi_lbl,   "...");
        lv_label_set_text(s_lq_lbl,     "...");
        lv_label_set_text(s_snr_lbl,    "...");
        lv_label_set_text(s_frames_lbl, "...");
        lv_label_set_text(s_rf_lbl,     "...");
        for (int i = 0; i < 8; i++) {
            if (s_ch_bars[i]) lv_bar_set_value(s_ch_bars[i], 0, LV_ANIM_OFF);
            if (s_ch_vals[i]) lv_label_set_text(s_ch_vals[i], "...");
        }
        return;
    }

    const auto &st = CRSFService::state();
    char buf[32];

    if (st.connected) {
        lv_label_set_text(s_state_lbl, "CONNECTED");
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0x06A77D), 0);
    } else {
        lv_label_set_text(s_state_lbl, "no signal");
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0xE6A23C), 0);
    }

    if (st.link.valid) {
        snprintf(buf, sizeof(buf), "%d dBm", -(int)st.link.uplink_rssi1);
        lv_label_set_text(s_rssi_lbl, buf);
        snprintf(buf, sizeof(buf), "%u %%", (unsigned)st.link.uplink_link_quality);
        lv_label_set_text(s_lq_lbl, buf);
        snprintf(buf, sizeof(buf), "%d dB", (int)st.link.uplink_snr);
        lv_label_set_text(s_snr_lbl, buf);
        snprintf(buf, sizeof(buf), "mode %u", (unsigned)st.link.rf_mode);
        lv_label_set_text(s_rf_lbl, buf);
    }

    snprintf(buf, sizeof(buf), "%lu (crc:%lu)",
             (unsigned long)st.total_frames, (unsigned long)st.bad_crc);
    lv_label_set_text(s_frames_lbl, buf);

    if (st.channels.valid) {
        for (int i = 0; i < 8; i++) {
            int v = st.channels.ch[i];
            int pct = ((v - 172) * 100) / (1811 - 172);
            if (pct < 0)   pct = 0;
            if (pct > 100) pct = 100;
            lv_bar_set_value(s_ch_bars[i], pct, LV_ANIM_OFF);
            snprintf(buf, sizeof(buf), "%d", crsfToUs(v));
            lv_label_set_text(s_ch_vals[i], buf);
        }
    }
}

// ---- Event handlers -------------------------------------------------------

static void startStopClicked(lv_event_t * /*e*/) {
    if (CRSFService::isRunning()) {
        CRSFService::end();
        CRSFConfig::reset();
        WebState::crsf.markStopped();
        PinPort::release(PinPort::PORT_B);
        Safety::logf("[elrs] CRSF stopped");
    } else {
        if (!PinPort::acquire(PinPort::PORT_B, PORT_UART, "elrs_ui")) {
            const char *mode  = PinPort::modeName(PinPort::currentMode(PinPort::PORT_B));
            const char *owner = PinPort::currentOwner(PinPort::PORT_B);
            Safety::logf("[elrs] Port B busy (mode=%s owner=%s)", mode, owner);
            if (s_state_lbl) {
                char buf[48];
                snprintf(buf, sizeof(buf), "Port B busy: %s", mode);
                lv_label_set_text(s_state_lbl, buf);
                lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0xE63946), 0);
            }
            return;
        }
        CRSFService::begin(&Serial1,
                           PinPort::rx_pin(PinPort::PORT_B),
                           PinPort::tx_pin(PinPort::PORT_B),
                           420000, false);
        CRSFConfig::init();
        WebState::crsf.markStarted(false);
        Safety::logf("[elrs] CRSF started (rx=%d tx=%d)",
            PinPort::rx_pin(PinPort::PORT_B),
            PinPort::tx_pin(PinPort::PORT_B));
    }
    elrsTick(nullptr);   // refresh immediately so the button label flips
}

static void bindClicked(lv_event_t * /*e*/) {
    if (!CRSFService::isRunning()) return;
    bool ok = CRSFService::cmdRxBind();
    Safety::logf("[elrs] bind sent ok=%d", (int)ok);
}

// ---- Section builders -----------------------------------------------------

static void buildStatusCard(lv_obj_t *parent) {
    lv_obj_t *card = makeCard(parent, "Link");
    s_state_lbl  = makeKvRow(card, "State");
    s_rssi_lbl   = makeKvRow(card, "RSSI");
    s_lq_lbl     = makeKvRow(card, "LQ");
    s_snr_lbl    = makeKvRow(card, "SNR");
    s_frames_lbl = makeKvRow(card, "Frames");
    s_rf_lbl     = makeKvRow(card, "RF mode");
}

static void buildChannelsCard(lv_obj_t *parent) {
    lv_obj_t *card = makeCard(parent, "Channels (CH1-CH8)");
    for (int i = 0; i < 8; i++) makeChannelRow(card, i);
}

static void buildActionRow(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 50);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row, 6, LV_PART_MAIN);

    s_start_btn = lv_button_create(row);
    lv_obj_set_flex_grow(s_start_btn, 1);
    lv_obj_set_height(s_start_btn, 50);
    lv_obj_set_style_radius(s_start_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(s_start_btn, startStopClicked, LV_EVENT_CLICKED, nullptr);
    s_start_lbl = lv_label_create(s_start_btn);
    lv_label_set_text(s_start_lbl, "Start CRSF");
    lv_obj_set_style_text_color(s_start_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_start_lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(s_start_lbl);

    s_bind_btn = lv_button_create(row);
    lv_obj_set_flex_grow(s_bind_btn, 1);
    lv_obj_set_height(s_bind_btn, 50);
    lv_obj_set_style_radius(s_bind_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bind_btn, lv_color_hex(0x394150), LV_PART_MAIN);
    lv_obj_add_event_cb(s_bind_btn, bindClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(s_bind_btn);
    lv_label_set_text(bl, "Bind");
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_18, 0);
    lv_obj_center(bl);
    lv_obj_add_flag(s_bind_btn, LV_OBJ_FLAG_HIDDEN);  // shown when running
}

// ---- Public entry ---------------------------------------------------------

void buildElrs(lv_obj_t *panel) {
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(panel, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(panel, elrsCleanup, LV_EVENT_DELETE, nullptr);

    buildStatusCard(panel);
    buildActionRow(panel);     // before channels so Start/Bind stay above the fold
    buildChannelsCard(panel);

    elrsTick(nullptr);   // populate once immediately

    // Prior tick is killed by elrsCleanup when the panel is destroyed.
    s_tick = lv_timer_create(elrsTick, 250, nullptr);   // 4 Hz
}

} // namespace screens
