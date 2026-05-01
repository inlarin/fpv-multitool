// Implementation: see port_modal.h

#include "port_modal.h"
#include "safety.h"

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

namespace {

lv_obj_t  *s_modal       = nullptr;
PortMode   s_target_mode = PORT_IDLE;
char       s_owner[24]   = {0};
PortModal::Continuation s_continuation = nullptr;

const char *modeNiceName(PortMode m) {
    switch (m) {
        case PORT_I2C:  return "I2C";
        case PORT_UART: return "UART";
        case PORT_PWM:  return "PWM";
        case PORT_GPIO: return "GPIO";
        default:        return "IDLE";
    }
}

void closeModal() {
    if (s_modal) {
        lv_obj_delete(s_modal);
        s_modal = nullptr;
    }
}

void switchClicked(lv_event_t * /*e*/) {
    Safety::logf("[port_modal] switch -> %s for '%s'",
                 PinPort::modeName(s_target_mode), s_owner);
    PinPort::release(PinPort::PORT_B);
    PinPort::acquire(PinPort::PORT_B, s_target_mode, s_owner);
    PinPort::setPreferredMode(PinPort::PORT_B, s_target_mode);

    PortModal::Continuation cont = s_continuation;
    s_continuation = nullptr;
    closeModal();
    if (cont) cont();
}

void cancelClicked(lv_event_t * /*e*/) {
    Safety::logf("[port_modal] cancel (continuation skipped)");
    s_continuation = nullptr;
    closeModal();
}

void buildModal(PortMode cur, PortMode needed, const char *owner_label) {
    closeModal();   // defensive

    s_modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_modal, 320, 480 - 24);
    lv_obj_set_pos(s_modal, 0, 24);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_modal, 16, LV_PART_MAIN);
    lv_obj_set_layout(s_modal, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_modal, 12, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(s_modal);
    char tbuf[40];
    snprintf(tbuf, sizeof(tbuf), "Port B not in %s", modeNiceName(needed));
    lv_label_set_text(title, tbuf);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE6A23C), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);

    lv_obj_t *msg = lv_label_create(s_modal);
    lv_obj_set_width(msg, lv_pct(100));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    const char *cur_owner = PinPort::currentOwner(PinPort::PORT_B);
    char buf[200];
    if (cur_owner && *cur_owner) {
        snprintf(buf, sizeof(buf),
                 "This screen needs Port B in %s mode, but it is currently "
                 "in %s (held by '%s'). Switch to %s now? Whatever owns "
                 "the port will be released.",
                 modeNiceName(needed), modeNiceName(cur),
                 cur_owner, modeNiceName(needed));
    } else {
        snprintf(buf, sizeof(buf),
                 "This screen needs Port B in %s mode, but it is currently "
                 "in %s. Switch to %s now?",
                 modeNiceName(needed), modeNiceName(cur), modeNiceName(needed));
    }
    lv_label_set_text(msg, buf);

    // Spacer pushes button row to the bottom.
    lv_obj_t *spacer = lv_obj_create(s_modal);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_size(spacer, 0, 1);

    lv_obj_t *btn_row = lv_obj_create(s_modal);
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
    lv_obj_add_event_cb(cancel, cancelClicked, LV_EVENT_CLICKED, nullptr);
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
    lv_obj_add_event_cb(sw, switchClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *swl = lv_label_create(sw);
    char swbuf[24];
    snprintf(swbuf, sizeof(swbuf), "Switch to %s", modeNiceName(needed));
    lv_label_set_text(swl, swbuf);
    lv_obj_set_style_text_color(swl, lv_color_white(), 0);
    lv_obj_set_style_text_font(swl, &lv_font_montserrat_18, 0);
    lv_obj_center(swl);
}

} // namespace

bool PortModal::ensureMode(PortMode needed,
                           const char *owner_label,
                           Continuation onReady) {
    PortMode cur = PinPort::currentMode(PinPort::PORT_B);
    if (cur == needed) {
        if (onReady) onReady();
        return true;
    }

    s_target_mode = needed;
    s_continuation = onReady;
    if (owner_label) {
        strncpy(s_owner, owner_label, sizeof(s_owner) - 1);
        s_owner[sizeof(s_owner) - 1] = '\0';
    } else {
        s_owner[0] = '\0';
    }
    buildModal(cur, needed, owner_label);
    return false;
}

void PortModal::close() {
    s_continuation = nullptr;
    closeModal();
}

bool PortModal::isOpen() { return s_modal != nullptr; }
