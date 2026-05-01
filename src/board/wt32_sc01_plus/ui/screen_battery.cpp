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
//   Service card : Sealed/Unsealed badge + Unseal / Clear PF /
//                  Cycle reset / Seal buttons + last-result line
//
// 2 Hz tick refreshes the labels; readAll() is ~50ms per call so
// faster polling would steal too much from the LVGL render budget.
//
// Service ops modify the battery's BMS persistently. They're behind
// the explicit unseal step (BMS rejects clearPF/cycle-reset/seal
// while sealed), so accidental damage is bounded.

#include "screens.h"

#include <Arduino.h>

#include "core/pin_port.h"
#include "battery/dji_battery.h"
#include "battery/smbus.h"   // SMBus::macCommand for DJI cycle-reset subcommands
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

// Service card live state.
static lv_obj_t *s_svc_seal_lbl = nullptr;     // "SEALED" / "UNSEALED"
static lv_obj_t *s_svc_result   = nullptr;     // last-op feedback
static lv_obj_t *s_svc_btn_unseal  = nullptr;
static lv_obj_t *s_svc_btn_clearpf = nullptr;
static lv_obj_t *s_svc_btn_cycle   = nullptr;
static lv_obj_t *s_svc_btn_seal    = nullptr;

// Port-mismatch modal (shown on entry if Port B is in UART/PWM/GPIO).
static lv_obj_t *s_port_modal = nullptr;

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

// Fires when the panel is deleted on Back. Kills only OUR tick + nulls
// our static widget pointers. LVGL's internal timers MUST not be touched.
static void batteryCleanup(lv_event_t * /*e*/) {
    if (s_tick) { lv_timer_delete(s_tick); s_tick = nullptr; }
    if (s_port_modal) {
        lv_obj_delete(s_port_modal);
        s_port_modal = nullptr;
    }
    s_state_lbl = s_mfr_lbl = s_dev_lbl = s_cycle_lbl = s_soh_lbl = nullptr;
    s_volt_lbl = s_curr_lbl = s_temp_lbl = s_soc_lbl = s_cap_lbl = nullptr;
    for (int i = 0; i < 4; i++) s_cell_lbls[i] = nullptr;
    s_svc_seal_lbl = s_svc_result = nullptr;
    s_svc_btn_unseal = s_svc_btn_clearpf = nullptr;
    s_svc_btn_cycle  = s_svc_btn_seal    = nullptr;
}

// Forward decl -- defined down in the Service section block.
static void setBtnEnabled(lv_obj_t *btn, bool enabled);

// Service card button states + seal badge -- called from batteryTick
// once per refresh cycle so the user sees the badge flip after an
// unseal/seal op without waiting for a manual reload.
static void serviceRefresh(const BatteryInfo &info) {
    if (!s_svc_seal_lbl) return;
    if (!info.connected) {
        lv_label_set_text(s_svc_seal_lbl, "no battery");
        lv_obj_set_style_text_color(s_svc_seal_lbl, lv_color_hex(0xa0a0a0), 0);
        setBtnEnabled(s_svc_btn_unseal,  false);
        setBtnEnabled(s_svc_btn_clearpf, false);
        setBtnEnabled(s_svc_btn_cycle,   false);
        setBtnEnabled(s_svc_btn_seal,    false);
        return;
    }
    if (info.sealed) {
        lv_label_set_text(s_svc_seal_lbl, "SEALED");
        lv_obj_set_style_text_color(s_svc_seal_lbl, lv_color_hex(0xE6A23C), 0);
        setBtnEnabled(s_svc_btn_unseal,  true);
        setBtnEnabled(s_svc_btn_clearpf, false);
        setBtnEnabled(s_svc_btn_cycle,   false);
        setBtnEnabled(s_svc_btn_seal,    false);
    } else {
        lv_label_set_text(s_svc_seal_lbl, "UNSEALED");
        lv_obj_set_style_text_color(s_svc_seal_lbl, lv_color_hex(0x06A77D), 0);
        setBtnEnabled(s_svc_btn_unseal,  true);   // still allowed, no-op if already
        setBtnEnabled(s_svc_btn_clearpf, true);
        setBtnEnabled(s_svc_btn_cycle,   true);
        setBtnEnabled(s_svc_btn_seal,    true);
    }
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
    BatteryInfo empty = {};   // for serviceRefresh in not-connected branches

    PortMode pm = PinPort::currentMode(PinPort::PORT_B);
    if (pm != PORT_I2C) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Port B is %s, switch to I2C",
                 PinPort::modeName(pm));
        lv_label_set_text(s_state_lbl, buf);
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0xE6A23C), 0);
        clearLabels();
        serviceRefresh(empty);
        return;
    }

    if (!DJIBattery::isConnected()) {
        lv_label_set_text(s_state_lbl, "no battery on bus");
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0xa0a0a0), 0);
        clearLabels();
        serviceRefresh(empty);
        return;
    }

    BatteryInfo info = DJIBattery::readAll();
    if (!info.connected) {
        lv_label_set_text(s_state_lbl, "battery dropped");
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(0xE6A23C), 0);
        clearLabels();
        serviceRefresh(empty);
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

    serviceRefresh(info);
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

// ---- Service section ------------------------------------------------------
//
// 4 ops:
//   Unseal      -- DJIBattery::unseal() tries every key in the model's
//                  profile. No-op (returns OK) if already unsealed.
//   Clear PF    -- DJIBattery::clearPFProper() + clearDJIPF2(). Resets
//                  Permanent-Failure flags so a falsely-tripped pack
//                  starts charging/discharging again.
//   Cycle reset -- DJI-specific MAC pair (0x6A28 then 0x6A2A) sent via
//                  ManufacturerAccess. Zeroes the cycle counter.
//   Seal        -- restores sealed mode, locks out further extended ops.
//
// Buttons other than Unseal stay greyed out while the BMS reports
// sealed=true; they would just bounce off the hardware otherwise.

static void serviceShow(const char *msg, uint16_t color) {
    if (!s_svc_result) return;
    lv_label_set_text(s_svc_result, msg);
    lv_obj_set_style_text_color(s_svc_result, lv_color_hex(color), 0);
    Safety::logf("[battery] %s", msg);
}

static void setBtnEnabled(lv_obj_t *btn, bool enabled) {
    if (!btn) return;
    if (enabled) {
        lv_obj_clear_state(btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x394150), LV_PART_MAIN);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x252a31), LV_PART_MAIN);
    }
}

static void unsealClicked(lv_event_t * /*e*/) {
    UnsealResult r = DJIBattery::unseal();
    switch (r) {
        case UNSEAL_OK:
            serviceShow("Unseal OK", 0x06A77D); break;
        case UNSEAL_REJECTED_SEALED:
            serviceShow("Unseal rejected (key wrong)", 0xE63946); break;
        case UNSEAL_NO_RESPONSE:
            serviceShow("Unseal: no I2C response", 0xE63946); break;
        case UNSEAL_UNSUPPORTED_MODEL:
            serviceShow("Unseal: no key for this model", 0xE6A23C); break;
    }
}

static void clearPfClicked(lv_event_t * /*e*/) {
    bool a = DJIBattery::clearPFProper();
    bool b = DJIBattery::clearDJIPF2();
    char buf[64];
    snprintf(buf, sizeof(buf), "PF clear: std=%s dji=%s",
             a ? "OK" : "FAIL", b ? "OK" : "FAIL");
    serviceShow(buf, (a && b) ? 0x06A77D : 0xE6A23C);
}

static void cycleResetClicked(lv_event_t * /*e*/) {
    // DJI-specific 2-step ManufacturerAccess sequence. Verified on
    // Mavic Air 2 / Mavic 2 packs. Pack must be unsealed; if either
    // write NACKs the pack is either sealed, sealed-different, or
    // not a DJI BMS at all.
    constexpr uint8_t  ADDR = 0x0B;
    constexpr uint16_t SUB1 = 0x6A28;
    constexpr uint16_t SUB2 = 0x6A2A;
    bool a = SMBus::macCommand(ADDR, SUB1);
    bool b = a && SMBus::macCommand(ADDR, SUB2);
    if (a && b) {
        serviceShow("Cycle reset SENT", 0x06A77D);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Cycle reset: step%d failed", a ? 2 : 1);
        serviceShow(buf, 0xE63946);
    }
}

static void sealClicked(lv_event_t * /*e*/) {
    bool ok = DJIBattery::seal();
    serviceShow(ok ? "Sealed" : "Seal failed", ok ? 0x06A77D : 0xE63946);
}

static lv_obj_t *makeSvcButton(lv_obj_t *parent, const char *label,
                               lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, 40);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x394150), LV_PART_MAIN);
    lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    return b;
}

// ---- Port-mismatch modal --------------------------------------------------
//
// On Battery screen entry, if Port B is held by a non-I2C consumer
// (UART for ELRS, PWM for servo / motor), the SBS reads will all NACK.
// Pop a modal that lets the user one-tap-switch to I2C without diving
// into Settings.

static void portModalClose() {
    if (s_port_modal) {
        lv_obj_delete(s_port_modal);
        s_port_modal = nullptr;
    }
}

static void portModalSwitchClicked(lv_event_t * /*e*/) {
    Safety::logf("[battery] modal switch -> I2C");
    PinPort::release(PinPort::PORT_B);
    PinPort::acquire(PinPort::PORT_B, PORT_I2C, "battery");
    PinPort::setPreferredMode(PinPort::PORT_B, PORT_I2C);
    DJIBattery::init();    // (re)acquire SMBus with the new pins
    portModalClose();
}

static void portModalCancelClicked(lv_event_t * /*e*/) {
    portModalClose();
}

static void showPortMismatchModal(PortMode cur, const char *owner) {
    if (s_port_modal) return;   // already open

    s_port_modal = lv_obj_create(lv_screen_active());
    // Sit BELOW the 24 px status bar -- same convention as the WiFi
    // edit modal in Settings, so the network state badge stays visible.
    lv_obj_set_size(s_port_modal, 320, 480 - 24);
    lv_obj_set_pos(s_port_modal, 0, 24);
    lv_obj_set_style_bg_color(s_port_modal, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_port_modal, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_port_modal, 16, LV_PART_MAIN);
    lv_obj_set_layout(s_port_modal, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_port_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_port_modal, 12, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(s_port_modal);
    lv_label_set_text(title, "Port B not in I2C");
    lv_obj_set_style_text_color(title, lv_color_hex(0xE6A23C), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);

    lv_obj_t *msg = lv_label_create(s_port_modal);
    lv_obj_set_width(msg, lv_pct(100));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    char buf[160];
    if (owner && *owner) {
        snprintf(buf, sizeof(buf),
                 "The battery bus needs Port B in I2C, but it is currently "
                 "in %s mode (held by '%s'). Switch to I2C now? "
                 "Whatever owns the port will be released.",
                 PinPort::modeName(cur), owner);
    } else {
        snprintf(buf, sizeof(buf),
                 "The battery bus needs Port B in I2C, but it is currently "
                 "in %s mode. Switch to I2C now?",
                 PinPort::modeName(cur));
    }
    lv_label_set_text(msg, buf);

    // Spacer so buttons sit at the bottom of the modal regardless of
    // message length.
    lv_obj_t *spacer = lv_obj_create(s_port_modal);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_size(spacer, 0, 1);

    lv_obj_t *btn_row = lv_obj_create(s_port_modal);
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
    lv_obj_add_event_cb(cancel, portModalCancelClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_18, 0);
    lv_obj_center(cl);

    lv_obj_t *sw = lv_button_create(btn_row);
    lv_obj_set_flex_grow(sw, 1);
    lv_obj_set_height(sw, 52);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x06A77D), LV_PART_MAIN);
    lv_obj_set_style_radius(sw, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(sw, portModalSwitchClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *swl = lv_label_create(sw);
    lv_label_set_text(swl, "Switch to I2C");
    lv_obj_set_style_text_color(swl, lv_color_white(), 0);
    lv_obj_set_style_text_font(swl, &lv_font_montserrat_18, 0);
    lv_obj_center(swl);
}

static void buildServiceCard(lv_obj_t *parent) {
    lv_obj_t *card = makeCard(parent, "Service");

    s_svc_seal_lbl = makeKvRow(card, "State");
    lv_label_set_text(s_svc_seal_lbl, "...");

    // 2x2 button grid (two flex rows).
    lv_obj_t *row1 = lv_obj_create(card);
    lv_obj_remove_style_all(row1);
    lv_obj_set_width(row1, lv_pct(100));
    lv_obj_set_height(row1, 44);
    lv_obj_set_layout(row1, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row1, 6, LV_PART_MAIN);

    s_svc_btn_unseal  = makeSvcButton(row1, "Unseal",   unsealClicked);
    s_svc_btn_clearpf = makeSvcButton(row1, "Clear PF", clearPfClicked);

    lv_obj_t *row2 = lv_obj_create(card);
    lv_obj_remove_style_all(row2);
    lv_obj_set_width(row2, lv_pct(100));
    lv_obj_set_height(row2, 44);
    lv_obj_set_layout(row2, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(row2, 6, LV_PART_MAIN);

    s_svc_btn_cycle = makeSvcButton(row2, "Cycle reset", cycleResetClicked);
    s_svc_btn_seal  = makeSvcButton(row2, "Seal",        sealClicked);

    s_svc_result = lv_label_create(card);
    lv_obj_set_width(s_svc_result, lv_pct(100));
    lv_obj_set_style_text_color(s_svc_result, lv_color_hex(0xa0a0a0), 0);
    lv_obj_set_style_text_font(s_svc_result, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_svc_result, "(no op yet)");
}

// ---- Public entry ---------------------------------------------------------

void buildBattery(lv_obj_t *panel) {
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(panel, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(panel, batteryCleanup, LV_EVENT_DELETE, nullptr);

    // DJIBattery::init() acquires Port B as I2C internally if the port
    // is IDLE. If it's held in another mode (UART/PWM/GPIO), init bails
    // -- in that case we pop a modal asking the user whether to release
    // the other owner and switch to I2C right now.
    DJIBattery::init();

    buildStatusCard(panel);
    buildPackCard(panel);
    buildChargeCard(panel);
    buildServiceCard(panel);

    // If init couldn't get the port into I2C, prompt. Done after building
    // the cards so the modal sits on top of a populated screen rather
    // than a blank one if the user cancels.
    PortMode pm = PinPort::currentMode(PinPort::PORT_B);
    if (pm != PORT_I2C) {
        showPortMismatchModal(pm, PinPort::currentOwner(PinPort::PORT_B));
    }

    batteryTick(nullptr);   // populate once immediately

    // Prior tick is killed by batteryCleanup when the panel is destroyed.
    s_tick = lv_timer_create(batteryTick, 500, nullptr);   // 2 Hz
}

} // namespace screens
