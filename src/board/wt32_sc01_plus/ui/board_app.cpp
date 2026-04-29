// BoardApp -- LVGL UI owner for the WT32-SC01 Plus full app.
//
// Wires lv_init + display + indev + the tabview-based bottom navigation.
// Tab content is delegated to per-screen builder functions (see
// screen_*.cpp). At Phase 1 every tab gets a placeholder; later phases
// replace those one at a time.
//
// We deliberately keep the LVGL <-> LovyanGFX plumbing local to this
// translation unit (and BoardDisplay::pushPixels) so the "include both"
// LVGL+LovyanGFX header collision (the lv_font/color.h enum redefine)
// never reaches application code.

#include "ui/board_app.h"
#include "board_display.h"

#include <Arduino.h>
#include <lvgl.h>

#include "screens.h"

// ---- LVGL bridges -----------------------------------------------------------

static BoardDisplay *s_display = nullptr;

static constexpr size_t LV_BUF_PIXELS = 480 * 32;
static lv_color_t      *s_buf_a   = nullptr;
static lv_display_t    *s_lv_disp  = nullptr;
static lv_indev_t      *s_lv_indev = nullptr;

static void lv_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)w * h);
    s_display->pushPixels(area->x1, area->y1, w, h, (const uint16_t *)px_map);
    lv_display_flush_ready(disp);
}

static void lv_indev_read_cb(lv_indev_t * /*indev*/, lv_indev_data_t *data) {
    UiTouch t = s_display->readTouch();
    if (t.pressed) {
        data->state   = LV_INDEV_STATE_PRESSED;
        data->point.x = t.x;
        data->point.y = t.y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static uint32_t lv_tick_cb() { return millis(); }

// ---- Bottom-bar tabview -----------------------------------------------------

static lv_obj_t *s_tabview = nullptr;

// Per-screen builders are defined in screen_<name>.cpp -- declared in
// ui/screens.h, dispatched by tab index here.
static const screens::TabSpec TABS[] = {
    { "HOME",    screens::buildHome     },
    { "SRV",     screens::buildServo    },
    { "MTR",     screens::buildMotor    },
    { "BAT",     screens::buildBattery  },
    { "RX",      screens::buildElrs     },
    { "SNIF",    screens::buildSniff    },
    { "CAT",     screens::buildCatalog  },
    { "SET",     screens::buildSettings },
};
static constexpr int NUM_TABS = sizeof(TABS) / sizeof(TABS[0]);

static void buildTabView() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);

    s_tabview = lv_tabview_create(scr);
    lv_tabview_set_tab_bar_position(s_tabview, LV_DIR_BOTTOM);
    lv_tabview_set_tab_bar_size(s_tabview, 56);

    // Style the tabview content area (above the bar) to match the dark theme.
    lv_obj_set_style_bg_color(s_tabview, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_bg_color(lv_tabview_get_content(s_tabview),
                              lv_color_hex(0x101418), LV_PART_MAIN);

    // Tab buttons strip: subtle highlight on the active tab.
    lv_obj_t *bar = lv_tabview_get_tab_bar(s_tabview);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1a1f24), LV_PART_MAIN);
    lv_obj_set_style_text_color(bar, lv_color_hex(0xa0a0a0), LV_PART_MAIN);
    lv_obj_set_style_text_color(bar, lv_color_hex(0xffffff), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(bar, &lv_font_montserrat_14, LV_PART_MAIN);

    // Build each tab's content using its registered builder.
    for (int i = 0; i < NUM_TABS; i++) {
        lv_obj_t *tab = lv_tabview_add_tab(s_tabview, TABS[i].label);
        lv_obj_set_style_bg_color(tab, lv_color_hex(0x101418), LV_PART_MAIN);
        TABS[i].builder(tab);
    }
}

// ---- Public API -------------------------------------------------------------

void BoardApp::begin(BoardDisplay &display) {
    _display  = &display;
    s_display = &display;

    lv_init();
    lv_tick_set_cb(lv_tick_cb);

    s_buf_a = (lv_color_t *)heap_caps_malloc(
        LV_BUF_PIXELS * sizeof(lv_color_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

    s_lv_disp = lv_display_create(display.width(), display.height());
    lv_display_set_flush_cb(s_lv_disp, lv_flush_cb);
    lv_display_set_buffers(s_lv_disp, s_buf_a, nullptr,
                           LV_BUF_PIXELS * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_lv_indev = lv_indev_create();
    lv_indev_set_type(s_lv_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_lv_indev, lv_indev_read_cb);

    buildTabView();
}

void BoardApp::loop() {
    lv_timer_handler();
}
