// WT32-SC01 Plus -- Sprint 32 step 2b LVGL + FT6336 touch sanity sketch.
//
// Built ONLY by [env:wt32_sc01_plus_lvgl]. Validates the full stack:
//   LovyanGFX driver -> BoardDisplay facade -> LVGL flush + indev.
//
// UI:
//   - Top status bar: rotation + raw touch coords + transformed coords + tap counter
//   - Center: a big "Tap me" button. Each tap recolors it (cycle of 4 colors)
//             and increments the counter. Confirms LVGL event delivery and
//             that touch coords land where the user tapped.
//   - Bottom row: 4 small buttons "rot 0", "rot 1", "rot 2", "rot 3". Tap
//             switches user-facing rotation through BoardDisplay::setRotation()
//             which (a) hardware-rotates LovyanGFX, (b) updates touch transform,
//             (c) persists to NVS, (d) tells LVGL the new resolution.
//
// On reboot the board comes up at the last-saved rotation -- if you flip
// to "rot 1" then power-cycle, you should see landscape mode again.
//
// What this proves before we move to the catalog UI:
//   - LVGL flush + DMA path on the i80 bus is healthy
//   - Touch coordinates stay aligned with the visible UI across all 4 rotations
//   - Settings persistence wiring is correct end-to-end

#include <Arduino.h>
#include <lvgl.h>

#include "board_display.h"
#include "board_settings.h"

static BoardDisplay s_display;

// LVGL partial buffer in INTERNAL SRAM (DMA-capable, cache-coherent
// with i80 reads). PSRAM was showing pixel-noise glyphs while solid
// fills rendered correctly -- classic "writes to PSRAM not flushed
// before LCD reads them" symptom. SRAM dodges that whole question.
//
// Size: 480 wide * 32 lines * 2 bytes = 30 KB. Lands in the 320 KB
// SRAM budget comfortably; LVGL's recommended floor is 1/10th of full
// screen pixels = 15360 px = 30 KB.
static constexpr size_t LV_BUF_PIXELS = 480 * 32;
static constexpr size_t LV_BUF_BYTES  = LV_BUF_PIXELS * 2;   // 16bpp
static uint8_t         *s_buf_a       = nullptr;

static lv_display_t    *s_lv_disp = nullptr;
static lv_indev_t      *s_lv_indev = nullptr;

// Forward refs for UI bits we need to update from event handlers.
static lv_obj_t *s_status_label = nullptr;
static lv_obj_t *s_tap_btn = nullptr;
static lv_obj_t *s_rot_btns[4] = {nullptr, nullptr, nullptr, nullptr};

static volatile uint32_t s_tap_count    = 0;
static volatile int16_t  s_last_x       = -1;
static volatile int16_t  s_last_y       = -1;

// ---- LVGL bridges -----------------------------------------------------------

// Push the dirty rect to the panel via the BoardDisplay facade.
// LVGL renders in native (little-endian) RGB565; ST7796's 16-bit pixel
// mode expects big-endian RGB565 on the wire. We byte-swap with the
// LVGL-provided helper -- it runs AFTER glyph blending so text glyphs
// stay correct (the LVGL SWAPPED-rendering path bugs out on AA text
// in v9.2). Then pushPixels flushes the already-swapped bytes raw.
static void lv_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)w * h);
    s_display.pushPixels(area->x1, area->y1, w, h, (const uint16_t *)px_map);
    lv_display_flush_ready(disp);
}

// Poll FT6336 once per LVGL indev tick. LVGL handles debouncing/long-press
// timing internally; we just say "is a finger down right now and where".
static void lv_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    UiTouch t = s_display.readTouch();
    if (t.pressed) {
        data->state  = LV_INDEV_STATE_PRESSED;
        data->point.x = t.x;
        data->point.y = t.y;
        s_last_x      = t.x;
        s_last_y      = t.y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// LVGL needs millisecond ticks. Arduino's millis() is the obvious source.
static uint32_t lv_tick_cb() {
    return millis();
}

// ---- UI ---------------------------------------------------------------------

static const lv_color_t TAP_COLORS[] = {
    lv_color_hex(0x2E86AB),   // teal
    lv_color_hex(0xE63946),   // red
    lv_color_hex(0xF1C453),   // amber
    lv_color_hex(0x06A77D),   // green
};

static void tap_btn_event_cb(lv_event_t *e) {
    s_tap_count++;
    lv_color_t c = TAP_COLORS[s_tap_count % (sizeof(TAP_COLORS) / sizeof(TAP_COLORS[0]))];
    lv_obj_set_style_bg_color(s_tap_btn, c, LV_PART_MAIN);
    Serial.printf("[tap] count=%lu  ui=(%d,%d)\n",
                  (unsigned long)s_tap_count, s_last_x, s_last_y);
}

extern void buildUI();

static void rot_btn_event_cb(lv_event_t *e) {
    uint8_t rot = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    s_display.setRotation(rot);
    // Tell LVGL the new logical resolution -- swaps w/h between
    // portrait and landscape and triggers a re-layout pass.
    lv_display_set_resolution(s_lv_disp, s_display.width(), s_display.height());
    buildUI();
    Serial.printf("[rot] -> %u  (lcd %dx%d)\n",
                  rot, s_display.width(), s_display.height());
}

void buildUI() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);

    int32_t W = s_display.width();
    int32_t H = s_display.height();

    // ---- Status bar (top) ----
    s_status_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 8, 6);
    lv_label_set_text(s_status_label, "rot=? ui=(?,?) taps=0");

    // ---- Big "Tap me" button (center) ----
    s_tap_btn = lv_button_create(scr);
    int32_t bw = (int32_t)(W * 0.7f);
    int32_t bh = (int32_t)(H * 0.45f);
    lv_obj_set_size(s_tap_btn, bw, bh);
    lv_obj_align(s_tap_btn, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(s_tap_btn, TAP_COLORS[s_tap_count % 4], LV_PART_MAIN);
    lv_obj_set_style_radius(s_tap_btn, 16, LV_PART_MAIN);
    lv_obj_add_event_cb(s_tap_btn, tap_btn_event_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *tap_label = lv_label_create(s_tap_btn);
    lv_label_set_text(tap_label, "Tap me");
    lv_obj_set_style_text_font(tap_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(tap_label, lv_color_white(), 0);
    lv_obj_center(tap_label);

    // ---- Rotation switcher (bottom) ----
    int32_t rb_h    = 50;
    int32_t rb_y    = H - rb_h - 10;
    int32_t rb_w    = (W - 8 * 5) / 4;     // 4 buttons + 5 gaps of 8 px
    for (int i = 0; i < 4; i++) {
        s_rot_btns[i] = lv_button_create(scr);
        lv_obj_set_size(s_rot_btns[i], rb_w, rb_h);
        lv_obj_set_pos(s_rot_btns[i], 8 + i * (rb_w + 8), rb_y);
        bool active = (s_display.rotation() == i);
        lv_obj_set_style_bg_color(s_rot_btns[i],
                                  active ? lv_color_hex(0x06A77D) : lv_color_hex(0x394150),
                                  LV_PART_MAIN);
        lv_obj_set_style_radius(s_rot_btns[i], 8, LV_PART_MAIN);
        lv_obj_add_event_cb(s_rot_btns[i], rot_btn_event_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        lv_obj_t *lbl = lv_label_create(s_rot_btns[i]);
        char buf[8];
        snprintf(buf, sizeof(buf), "rot %d", i);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_center(lbl);
    }
}

// ---- Boot --------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 2000) { delay(10); }

    Serial.println();
    Serial.println(F("================ WT32-SC01 Plus LVGL sanity ================"));

    BoardSettings::begin();
    bool touch_ok = s_display.begin();
    Serial.printf("  rotation (from NVS) : %u\n", s_display.rotation());
    Serial.printf("  touch (FT6336 0x38) : %s\n", touch_ok ? "ACK" : "TIMEOUT");
    Serial.printf("  panel               : %d x %d (current rotation)\n",
                  s_display.width(), s_display.height());

    // ---- LVGL init ----
    lv_init();
    lv_tick_set_cb(lv_tick_cb);

    // 30 KB partial buffer in INTERNAL SRAM, DMA-capable + cache-coherent.
    s_buf_a = (uint8_t *)heap_caps_malloc(LV_BUF_BYTES,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_buf_a) {
        Serial.println("!!! Internal-SRAM alloc for LVGL buffer failed");
    }
    Serial.printf("  LVGL buffer        : %u bytes at %p (internal SRAM)\n",
                  (unsigned)LV_BUF_BYTES, s_buf_a);

    s_lv_disp = lv_display_create(s_display.width(), s_display.height());
    // Plain RGB565 (LVGL's default 16bpp). Byte swap to big-endian for
    // the ST7796 happens in flush_cb after blending.
    lv_display_set_flush_cb(s_lv_disp, lv_flush_cb);
    lv_display_set_buffers(s_lv_disp, s_buf_a, /* buf2 = */ nullptr,
                           LV_BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_lv_indev = lv_indev_create();
    lv_indev_set_type(s_lv_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_lv_indev, lv_indev_read_cb);

    buildUI();

    Serial.println(F("LVGL ready -- entering loop"));
    Serial.println(F("============================================================"));
}

void loop() {
    // Drive LVGL. ~5 ms cadence is the canonical rate for v9 partial-render.
    lv_timer_handler();

    // Refresh status label with the freshest touch + rotation data once
    // every ~120 ms (cheaper than every loop tick).
    static uint32_t last_status = 0;
    if (millis() - last_status >= 120 && s_status_label) {
        last_status = millis();
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "rot=%u  ui=(%d,%d)  taps=%lu",
                 s_display.rotation(),
                 (int)s_last_x, (int)s_last_y,
                 (unsigned long)s_tap_count);
        lv_label_set_text(s_status_label, buf);
    }

    // Heartbeat over USB so we can see the device hasn't hung.
    static uint32_t last_hb = 0;
    if (millis() - last_hb >= 5000) {
        last_hb = millis();
        Serial.printf("alive  rot=%u  taps=%lu  free heap=%u  free psram=%u\n",
                      s_display.rotation(), (unsigned long)s_tap_count,
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    }

    delay(5);
}
