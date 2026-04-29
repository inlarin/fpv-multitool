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

// ---- Synthetic touch queue --------------------------------------------------
//
// Used by /api/sys/ui/tap to drive the UI off-board (no human finger
// required). Enqueued in pairs: PRESSED-at-(x,y) immediately, then
// RELEASED-at-(x,y) ~60 ms later. The indev_read_cb pulls from this
// queue BEFORE polling FT6336, so for the 60 ms window LVGL sees a
// finger at the synthetic coordinates. That gives it enough ticks
// (3-4 polls at the default 33 ms LVGL frame) to fire press_lost +
// click events naturally.
//
// Fixed-size ring (8 events = 4 taps queued max). Overrun returns
// false from injectTap so the caller can back off.

struct SynthEvt {
    int16_t  x, y;
    bool     pressed;
    uint32_t when_ms;
};
static constexpr int SYNTH_CAP = 16;
static SynthEvt  s_synth[SYNTH_CAP];
static int       s_synth_head = 0;
static int       s_synth_tail = 0;

static int synthCount() {
    return (s_synth_tail - s_synth_head + SYNTH_CAP) % SYNTH_CAP;
}

bool BoardApp::injectTap(int16_t x, int16_t y) {
    // Need 2 free slots (PRESS + RELEASE).
    if (synthCount() + 2 >= SYNTH_CAP) return false;
    uint32_t now = millis();
    s_synth[s_synth_tail] = { x, y, true,  now };
    s_synth_tail = (s_synth_tail + 1) % SYNTH_CAP;
    s_synth[s_synth_tail] = { x, y, false, now + 60 };
    s_synth_tail = (s_synth_tail + 1) % SYNTH_CAP;
    return true;
}

// Sticky state -- once a synthetic PRESSED event is delivered, keep
// reporting "still pressed at the same point" to LVGL until the matching
// RELEASE event time arrives. Without this, a synthetic press could be
// "interrupted" by a real-touch poll that returns RELEASED, breaking
// the click sequence.
static bool     s_synth_active = false;
static int16_t  s_synth_x = 0, s_synth_y = 0;
static uint32_t s_synth_release_at = 0;

static void lv_indev_read_cb(lv_indev_t * /*indev*/, lv_indev_data_t *data) {
    uint32_t now = millis();

    // Drain any synthetic events whose `when_ms` has come due. Process
    // them in order; the latest determines current state.
    while (synthCount() > 0 && s_synth[s_synth_head].when_ms <= now) {
        SynthEvt &e = s_synth[s_synth_head];
        if (e.pressed) {
            s_synth_active     = true;
            s_synth_x          = e.x;
            s_synth_y          = e.y;
            s_synth_release_at = 0;
        } else {
            s_synth_release_at = e.when_ms;
        }
        s_synth_head = (s_synth_head + 1) % SYNTH_CAP;
    }

    // While the synthetic press is active, override real touch.
    if (s_synth_active) {
        if (s_synth_release_at != 0 && now >= s_synth_release_at) {
            // Deliver RELEASE once, then clear the active flag.
            data->state   = LV_INDEV_STATE_RELEASED;
            data->point.x = s_synth_x;
            data->point.y = s_synth_y;
            s_synth_active = false;
            return;
        }
        data->state   = LV_INDEV_STATE_PRESSED;
        data->point.x = s_synth_x;
        data->point.y = s_synth_y;
        return;
    }

    // No synthetic event in flight -- normal FT6336 path.
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

// ---- Status bar chrome (top 32 px) ------------------------------------------
//
// Visible on every tab. Shows: WiFi icon + IP, uptime, free heap,
// optional inline "working" pill driven by BoardApp::setBusy/clearBusy.
// Refreshed at 1 Hz via lv_timer; bumped to ~10 Hz while a busy op
// holds the pill visible.

#include <WiFi.h>

static lv_obj_t *s_status_bar    = nullptr;
static lv_obj_t *s_status_wifi   = nullptr;   // icon + IP
static lv_obj_t *s_status_uptime = nullptr;
static lv_obj_t *s_status_heap   = nullptr;
static lv_obj_t *s_status_busy   = nullptr;   // hidden when not busy
static lv_obj_t *s_status_busy_lbl = nullptr;

static char     s_busy_text[64]  = {0};
static int      s_busy_progress  = -1;

static constexpr int STATUS_BAR_H = 24;

static void buildStatusBar(lv_obj_t *parent) {
    s_status_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(s_status_bar);
    lv_obj_set_size(s_status_bar, lv_pct(100), STATUS_BAR_H);
    lv_obj_set_pos(s_status_bar, 0, 0);
    lv_obj_set_style_bg_color(s_status_bar, lv_color_hex(0x1a1f24), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_status_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_status_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_status_bar, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_status_bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_status_wifi = lv_label_create(s_status_bar);
    lv_obj_set_style_text_color(s_status_wifi, lv_color_hex(0xa0a0a0), 0);
    lv_obj_set_style_text_font(s_status_wifi, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_status_wifi, LV_SYMBOL_WIFI " ...");
    lv_obj_set_style_pad_right(s_status_wifi, 12, 0);

    s_status_uptime = lv_label_create(s_status_bar);
    lv_obj_set_style_text_color(s_status_uptime, lv_color_hex(0xa0a0a0), 0);
    lv_obj_set_style_text_font(s_status_uptime, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_status_uptime, "0s");
    lv_obj_set_style_pad_right(s_status_uptime, 12, 0);

    s_status_heap = lv_label_create(s_status_bar);
    lv_obj_set_style_text_color(s_status_heap, lv_color_hex(0xa0a0a0), 0);
    lv_obj_set_style_text_font(s_status_heap, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_status_heap, "");
    lv_obj_set_style_pad_right(s_status_heap, 12, 0);

    // Spacer pushes busy pill (when shown) to the right edge.
    lv_obj_t *spacer = lv_obj_create(s_status_bar);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_size(spacer, 0, 1);

    s_status_busy = lv_obj_create(s_status_bar);
    lv_obj_remove_style_all(s_status_busy);
    lv_obj_set_size(s_status_busy, LV_SIZE_CONTENT, 18);
    lv_obj_set_style_bg_color(s_status_busy, lv_color_hex(0x2E86AB), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_status_busy, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_status_busy, 9, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_status_busy, 6, LV_PART_MAIN);
    lv_obj_add_flag(s_status_busy, LV_OBJ_FLAG_HIDDEN);

    s_status_busy_lbl = lv_label_create(s_status_busy);
    lv_obj_set_style_text_color(s_status_busy_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_status_busy_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_status_busy_lbl, "");
    lv_obj_center(s_status_busy_lbl);
}

static void formatUptime(uint32_t s, char *out, size_t n) {
    if (s < 60)        snprintf(out, n, "%us", (unsigned)s);
    else if (s < 3600) snprintf(out, n, "%um %us", (unsigned)(s / 60), (unsigned)(s % 60));
    else               snprintf(out, n, "%uh %um", (unsigned)(s / 3600),
                                  (unsigned)((s % 3600) / 60));
}

static void refreshStatusBar() {
    if (!s_status_bar) return;

    // WiFi: icon color + IP / SSID hint.
    bool sta = WiFi.status() == WL_CONNECTED;
    char buf[64];
    if (sta) {
        snprintf(buf, sizeof(buf),
                 LV_SYMBOL_WIFI " %s", WiFi.localIP().toString().c_str());
        lv_obj_set_style_text_color(s_status_wifi, lv_color_hex(0x06A77D), 0);
    } else if (WiFi.getMode() & WIFI_AP) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " AP");
        lv_obj_set_style_text_color(s_status_wifi, lv_color_hex(0xF1C453), 0);
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " --");
        lv_obj_set_style_text_color(s_status_wifi, lv_color_hex(0xE63946), 0);
    }
    lv_label_set_text(s_status_wifi, buf);

    // Uptime
    char ut[24];
    formatUptime(millis() / 1000, ut, sizeof(ut));
    lv_label_set_text(s_status_uptime, ut);

    // Heap (KB)
    snprintf(buf, sizeof(buf), "%uK", (unsigned)(ESP.getFreeHeap() / 1024));
    lv_label_set_text(s_status_heap, buf);

    // Busy pill
    if (s_busy_text[0] != '\0') {
        lv_obj_remove_flag(s_status_busy, LV_OBJ_FLAG_HIDDEN);
        if (s_busy_progress >= 0) {
            char b[80];
            snprintf(b, sizeof(b), LV_SYMBOL_LOOP " %s %d%%",
                     s_busy_text, s_busy_progress);
            lv_label_set_text(s_status_busy_lbl, b);
        } else {
            char b[80];
            snprintf(b, sizeof(b), LV_SYMBOL_LOOP " %s", s_busy_text);
            lv_label_set_text(s_status_busy_lbl, b);
        }
    } else {
        lv_obj_add_flag(s_status_busy, LV_OBJ_FLAG_HIDDEN);
    }
}

static void statusBarTick(lv_timer_t * /*t*/) {
    refreshStatusBar();
}

void BoardApp::setBusy(const char *label, int progress_pct) {
    if (!label) {
        s_busy_text[0]  = '\0';
        s_busy_progress = -1;
        return;
    }
    snprintf(s_busy_text, sizeof(s_busy_text), "%s", label);
    s_busy_progress = progress_pct;
}

void BoardApp::clearBusy() {
    s_busy_text[0]  = '\0';
    s_busy_progress = -1;
}

// ---- Springboard navigation -------------------------------------------------
//
// Home screen is a 2x4 grid of section tiles. Tap a tile -> the content
// area swaps to that section's full-screen view, with a "Back" button
// at the top. Tap Back -> returns to the home grid. Status bar stays
// pinned at the top across both views.
//
// Layout below the 32 px status bar:
//
//   +-----------------------------+ y=32
//   |   <home tile grid           |
//   |    OR section view          |
//   |    with back button>        |
//   +-----------------------------+ y=480

static lv_obj_t *s_content_area = nullptr;

static const screens::SectionSpec SECTIONS[] = {
    { "Servo",    LV_SYMBOL_REFRESH,       screens::buildServo    },
    { "Motor",    LV_SYMBOL_GPS,           screens::buildMotor    },
    { "Battery",  LV_SYMBOL_BATTERY_FULL,  screens::buildBattery  },
    { "ELRS RX",  LV_SYMBOL_WIFI,          screens::buildElrs     },
    { "RC Sniff", LV_SYMBOL_EYE_OPEN,      screens::buildSniff    },
    { "Catalog",  LV_SYMBOL_LIST,          screens::buildCatalog  },
    { "Settings", LV_SYMBOL_SETTINGS,      screens::buildSettings },
};
static constexpr int NUM_SECTIONS = sizeof(SECTIONS) / sizeof(SECTIONS[0]);

static void showHome();
static void showSection(int idx);

static void homeTileClicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    showSection(idx);
}

static void backClicked(lv_event_t * /*e*/) {
    showHome();
}

static void showHome() {
    lv_obj_clean(s_content_area);
    lv_obj_set_layout(s_content_area, LV_LAYOUT_NONE);

    // Explicit positioning -- LVGL flex_wrap was dropping every tile into
    // a single row regardless of the wrap setting (likely due to width
    // computation after lv_obj_remove_style_all). Manual math is rock
    // solid and makes the geometry trivial to reason about.
    //
    // Content area is 320 wide x (480 - status bar) tall.
    constexpr int CONTENT_W = 320;
    constexpr int CONTENT_H = 480 - STATUS_BAR_H;
    constexpr int PAD       = 12;
    constexpr int GAP       = 12;
    constexpr int TILE_W    = (CONTENT_W - 2 * PAD - GAP) / 2;        // 142
    constexpr int TILE_H    = (CONTENT_H - 2 * PAD - 3 * GAP) / 4;    // 97

    for (int i = 0; i < NUM_SECTIONS; i++) {
        int col = i % 2;
        int row = i / 2;
        int x   = PAD + col * (TILE_W + GAP);
        int y   = PAD + row * (TILE_H + GAP);

        lv_obj_t *tile = lv_obj_create(s_content_area);
        lv_obj_set_size(tile, TILE_W, TILE_H);
        lv_obj_set_pos(tile, x, y);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x1a1f24), LV_PART_MAIN);
        lv_obj_set_style_border_color(tile, lv_color_hex(0x394150), LV_PART_MAIN);
        lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(tile, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_all(tile, 8, LV_PART_MAIN);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        // Pressed tint for visual feedback.
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x2E86AB), LV_STATE_PRESSED);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, homeTileClicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lv_obj_t *icon = lv_label_create(tile);
        lv_obj_set_style_text_color(icon, lv_color_hex(0x2E86AB), 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
        lv_label_set_text(icon, SECTIONS[i].icon);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 2);

        lv_obj_t *lbl = lv_label_create(tile);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xE0E0E0), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        lv_label_set_text(lbl, SECTIONS[i].label);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -2);
    }
}

static void showSection(int idx) {
    if (idx < 0 || idx >= NUM_SECTIONS) return;
    lv_obj_clean(s_content_area);
    lv_obj_set_layout(s_content_area, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(s_content_area, 0, 0);

    // Top row: back button + section title
    lv_obj_t *back_row = lv_obj_create(s_content_area);
    lv_obj_remove_style_all(back_row);
    lv_obj_set_size(back_row, lv_pct(100), 40);
    lv_obj_set_pos(back_row, 0, 0);
    lv_obj_set_style_bg_color(back_row, lv_color_hex(0x1a1f24), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back_row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(back_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(back_row);
    lv_obj_set_size(back, 80, 40);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x394150), LV_PART_MAIN);
    lv_obj_set_style_radius(back, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(back, backClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Home");
    lv_obj_set_style_text_color(back_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(back_lbl);

    lv_obj_t *title = lv_label_create(back_row);
    lv_label_set_text(title, SECTIONS[idx].label);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_align(title, LV_ALIGN_RIGHT_MID, -8, 0);

    // Content panel below the back row -- this is what builders fill.
    lv_obj_t *panel = lv_obj_create(s_content_area);
    lv_obj_set_size(panel, lv_pct(100), 480 - STATUS_BAR_H - 40);
    lv_obj_set_pos(panel, 0, 40);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 12, 0);
    lv_obj_set_style_border_width(panel, 0, 0);

    SECTIONS[idx].builder(panel);
}

static void buildSpringboard() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Status bar at the top -- pinned across home/section swaps.
    buildStatusBar(scr);

    // Content area below the status bar.
    s_content_area = lv_obj_create(scr);
    lv_obj_remove_style_all(s_content_area);
    lv_obj_set_size(s_content_area, lv_pct(100), 480 - STATUS_BAR_H);
    lv_obj_set_pos(s_content_area, 0, STATUS_BAR_H);
    lv_obj_set_style_bg_color(s_content_area, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_content_area, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_content_area, LV_OBJ_FLAG_SCROLLABLE);

    showHome();
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

    buildSpringboard();

    // 1 Hz status-bar refresh. lv_timer fires from inside lv_timer_handler()
    // which we already drive from loop(), so no extra task / sync needed.
    lv_timer_create(statusBarTick, 1000, nullptr);
    refreshStatusBar();   // populate immediately so first frame isn't blank
}

void BoardApp::loop() {
    lv_timer_handler();
}
