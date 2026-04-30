// Battery screen -- quick read for any SBS-compliant smart battery.
//
// Talks to whatever responds at I2C addr 0x0B on Port B (DJI, Autel,
// generic SBS) via DJIBattery::readAll(). DJI-specific extras like
// PFStatus, DJI serial, and DJI PF2 are reported when the manufacturer
// matches; everything else still works for non-DJI packs.
//
// Layout (320x408 panel, scrollable column):
//   Status card  : State / Manufacturer / Device name / Cycles / SoH
//   Pack card    : Voltage (large hero) / Current / Temperature
//   Charge card  : SoC% / Remaining capacity / Cells 1-4 voltages
//
// 2 Hz tick refreshes the labels; readAll() is ~50ms per call so
// faster polling would steal too much from the LVGL render budget.

#include "screens.h"

#include <Arduino.h>

#include "core/pin_port.h"
#include "battery/dji_battery.h"
#include "safety.h"

namespace screens {

// ---- Live widgets ---------------------------------------------------------

static lv_obj_t *s_state_lbl   = nullptr;
static lv_obj_t *s_mfr_lbl     = nullptr;
static lv_obj_t *s_dev_lbl     = nullptr;
static lv_obj_t *s_cycle_lbl   = nullptr;
static lv_obj_t *s_soh_lbl     = nullptr;
static lv_obj_t *s_volt_lbl    = nullptr;   // hero voltage
static lv_obj_t *s_curr_lbl    = nullptr;
static lv_obj_t *s_temp_lbl    = nullptr;
static lv_obj_t *s_soc_lbl     = nullptr;
static lv_obj_t *s_cap_lbl     = nullptr;
static lv_obj_t *s_cell_lbls[4] = {nullptr};
static lv_timer_t *s_tick      = nullptr;

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

// ---- Live refresh ---------------------------------------------------------

static void clearLabels() {
    if (s_mfr_lbl)   lv_label_set_text(s_mfr_lbl,   "...");
    if (s_dev_lbl)   lv_label_set_text(s_dev_lbl,   "...");
    if (s_cycle_lbl) lv_label_set_text(s_cycle_lbl, "...");
    if (s_soh_lbl)   lv_label_set_text(s_soh_lbl,   "...");
    if (s_volt_lbl)  lv_label_set_text(s_volt_lbl,  "--.- V");
    if (s_curr_lbl)  lv_label_set_text(s_curr_lbl,  "...");
    if (s_temp_lbl)  lv_label_set_text(s_temp_lbl,  "...");
    if (s_soc_lbl)   lv_label_set_text(s_soc_lbl,   "...");
    if (s_cap_lbl)   lv_label_set_text(s_cap_lbl,   "...");
    for (int i = 0; i < 4; i++) {
        if (s_cell_lbls[i]) lv_label_set_text(s_cell_lbls[i], "...");
    }
}

static void batteryTick(lv_timer_t * /*t*/) {
    if (!s_state_lbl) return;

    // Show port mismatch instead of "no battery" -- saves the user a
    // confused trip to Settings when the issue is just Port B mode.
    PortMode pm = PinPort::currentMode(PinPort::PORT_B);
    if (pm != PORT_I2C) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Port B is %s, switch to I2C",
                 PinPort::modeName(pm));
        lv_label_set_text(s_state_lbl, buf);
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0xE6A23C), 0);
        clearLabels();
        return;
    }

    if (!DJIBattery::isConnected()) {
        lv_label_set_text(s_state_lbl, "no battery on bus");
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0xa0a0a0), 0);
        clearLabels();
        return;
    }

    BatteryInfo info = DJIBattery::readAll();
    if (!info.connected) {
        lv_label_set_text(s_state_lbl, "battery dropped");
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0xE6A23C), 0);
        clearLabels();
        return;
    }

    char buf[40];

    const char *kind = (info.deviceType == DEV_DJI_BATTERY) ? "DJI" : "SBS";
    snprintf(buf, sizeof(buf), "CONNECTED (%s, %us)", kind, (unsigned)info.cellCount);
    lv_label_set_text(s_state_lbl, buf);
    lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0x06A77D), 0);

    lv_label_set_text(s_mfr_lbl,
        info.manufacturerName.length() ? info.manufacturerName.c_str() : "—");
    lv_label_set_text(s_dev_lbl,
        info.deviceName.length() ? info.deviceName.c_str() : "—");

    snprintf(buf, sizeof(buf), "%u", (unsigned)info.cycleCount);
    lv_label_set_text(s_cycle_lbl, buf);
    snprintf(buf, sizeof(buf), "%u %%", (unsigned)info.stateOfHealth);
    lv_label_set_text(s_soh_lbl, buf);

    // Hero voltage in the pack card.
    snprintf(buf, sizeof(buf), "%.2f V", info.voltage_mV / 1000.0f);
    lv_label_set_text(s_volt_lbl, buf);

    // Current sign convention: + = discharging, - = charging (DJI BMS).
    snprintf(buf, sizeof(buf), "%+.2f A", info.current_mA / 1000.0f);
    lv_label_set_text(s_curr_lbl, buf);

    snprintf(buf, sizeof(buf), "%.1f C", info.temperature_C);
    lv_label_set_text(s_temp_lbl, buf);

    snprintf(buf, sizeof(buf), "%u %%", (unsigned)info.stateOfCharge);
    lv_label_set_text(s_soc_lbl, buf);

    snprintf(buf, sizeof(buf), "%u / %u mAh",
             (unsigned)info.remainCapacity_mAh, (unsigned)info.fullCapacity_mAh);
    lv_label_set_text(s_cap_lbl, buf);

    // Cells: prefer DAStatus1 sync data when available, fall back to
    // unsynced 0x3C-0x3F. Show only as many as the detected cell count.
    const uint16_t *cells =
        info.daStatus1Valid ? info.cellVoltSync : info.cellVoltage;
    for (int i = 0; i < 4; i++) {
        if (i < info.cellCount && cells[i] > 0) {
            snprintf(buf, sizeof(buf), "%.3f V", cells[i] / 1000.0f);
            lv_label_set_text(s_cell_lbls[i], buf);
        } else {
            lv_label_set_text(s_cell_lbls[i], "—");
        }
    }
}

// ---- Section builders -----------------------------------------------------

static void buildStatusCard(lv_obj_t *parent) {
    lv_obj_t *card = makeCard(parent, "Status");
    s_state_lbl = makeKvRow(card, "State");
    s_mfr_lbl   = makeKvRow(card, "Mfr");
    s_dev_lbl   = makeKvRow(card, "Model");
    s_cycle_lbl = makeKvRow(card, "Cycles");
    s_soh_lbl   = makeKvRow(card, "Health");
}

static void buildPackCard(lv_obj_t *parent) {
    lv_obj_t *card = makeCard(parent, "Pack");

    // Hero voltage spans the row, big readout. SoC card below for state.
    s_volt_lbl = lv_label_create(card);
    lv_obj_set_width(s_volt_lbl, lv_pct(100));
    lv_obj_set_style_text_color(s_volt_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_volt_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(s_volt_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_volt_lbl, "--.- V");

    s_curr_lbl = makeKvRow(card, "Current");
    s_temp_lbl = makeKvRow(card, "Temperature");
}

static void buildChargeCard(lv_obj_t *parent) {
    lv_obj_t *card = makeCard(parent, "Charge");
    s_soc_lbl = makeKvRow(card, "SoC");
    s_cap_lbl = makeKvRow(card, "Capacity");
    for (int i = 0; i < 4; i++) {
        char k[8]; snprintf(k, sizeof(k), "Cell %d", i + 1);
        s_cell_lbls[i] = makeKvRow(card, k);
    }
}

// ---- Public entry ---------------------------------------------------------

void buildBattery(lv_obj_t *panel) {
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(panel, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);

    // DJIBattery::init() acquires Port B as I2C internally and runs
    // SMBus::init on the right pins. It's idempotent: if Port B is already
    // I2C (boot-time autel_battery owner), it transfers ownership and
    // re-initialises SMBus harmlessly. If Port B is held in another mode
    // (UART/PWM), init() bails and the tick surfaces "Port B is X" so
    // the user can fix it from Settings rather than seeing a blank screen.
    DJIBattery::init();

    buildStatusCard(panel);
    buildPackCard(panel);
    buildChargeCard(panel);

    batteryTick(nullptr);   // populate once immediately

    if (s_tick) lv_timer_delete(s_tick);
    s_tick = lv_timer_create(batteryTick, 500, nullptr);   // 2 Hz
}

} // namespace screens
