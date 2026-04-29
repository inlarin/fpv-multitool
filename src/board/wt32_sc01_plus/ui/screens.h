#pragma once

// Per-screen builder declarations for the BoardApp tabview.
//
// Each builder receives the tab's content panel (already created by
// lv_tabview_add_tab) and populates it with widgets. The panel is full
// width of the screen and the height between the status bar (none yet)
// and the bottom navigation bar.

#include <lvgl.h>

namespace screens {

struct TabSpec {
    const char *label;                       // bottom-bar text (≤4 chars)
    void      (*builder)(lv_obj_t *tab);
};

void buildHome    (lv_obj_t *tab);
void buildServo   (lv_obj_t *tab);
void buildMotor   (lv_obj_t *tab);
void buildBattery (lv_obj_t *tab);
void buildElrs    (lv_obj_t *tab);
void buildSniff   (lv_obj_t *tab);
void buildCatalog (lv_obj_t *tab);
void buildSettings(lv_obj_t *tab);

} // namespace screens
