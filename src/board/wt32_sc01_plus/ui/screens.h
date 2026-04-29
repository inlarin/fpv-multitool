#pragma once

// Per-screen builder declarations for the BoardApp springboard.
//
// Each builder receives a content panel (already created by BoardApp
// inside a full-screen section view, beneath a back button + title)
// and populates it with widgets. Panel dimensions on a 320x480 portrait
// SC01 Plus: 320 wide x ~408 tall (480 - 32 status bar - 40 back-bar).

#include <lvgl.h>

namespace screens {

struct SectionSpec {
    const char *label;     // shown on the home tile + as the section title
    const char *icon;      // LVGL symbol string for the tile icon
    void      (*builder)(lv_obj_t *content_panel);
};

void buildServo   (lv_obj_t *panel);
void buildMotor   (lv_obj_t *panel);
void buildBattery (lv_obj_t *panel);
void buildElrs    (lv_obj_t *panel);
void buildSniff   (lv_obj_t *panel);
void buildCatalog (lv_obj_t *panel);
void buildSettings(lv_obj_t *panel);

} // namespace screens
