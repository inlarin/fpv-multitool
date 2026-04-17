// ESP32-S3-LCD-1.47B enclosure — parametric, closed-box design
// Pins soldered DOWN (9.5 mm tails) for Dupont. Wire bundle exits via a
// rectangular opening in the floor. Four small feet raise the case for
// clearance.
//
// Units: millimeters. Render: F6, File → Export → Export as STL.
//
// ─── PRINT ORIENTATION ────────────────────────────────────────────────────
//   TRAY: floor-down (natural orientation). Floor on the build plate, open
//         top facing up. No overhangs except the 10-mm USB-C bridge.
//   LID:  top-down (LCD window on the build plate, screw-counterbore face
//         down). No supports needed.
//
// ─── ASSEMBLY ─────────────────────────────────────────────────────────────
//   1. Plug all Dupont connectors onto the PCB pins first (outside the case).
//   2. Thread the wire bundle through the floor opening from above.
//   3. Lower PCB into the tray from the top; it rests on the 4 chamfered
//      corner tabs. LCD faces up.
//   4. Place lid, fasten with 4 × M2 × 8 mm self-tapping screws.
//
// ─── VERIFY ON REAL BOARD ─────────────────────────────────────────────────
//   - BOOT / RST Y-position on the short wall (boot_offset_from_usbc)
//   - LCD x-position on PCB (lcd_x_offset, tune after first placement)
//   - Dupont housing height (default 14 mm)

/* [Render] */
part = "both";  // [tray, lid, both]

/* [PCB] */
pcb_len = 36.37;
pcb_wid = 20.32;
pcb_thk = 1.60;

/* [Pin headers soldered DOWN] */
pin_total_below_pcb = 9.5;    // measured
pin_plastic_height  = 2.5;    // plastic body height below PCB
dupont_housing_h    = 14.0;   // Dupont female crimp body

/* [LCD — 1.47" 172×320] */
lcd_active_long  = 32.84;     // along PCB long axis
lcd_active_short = 17.64;     // along PCB short axis
lcd_module_thk   = 2.20;
lcd_x_offset     = 0.0;       // shift window along long axis if needed

/* [Enclosure] */
wall       = 1.80;
floor_thk  = 1.50;
lid_thk    = 1.80;
clearance  = 0.40;            // PCB ↔ inner wall gap (loose for easy insertion)
corner_r   = 2.50;

// Cavity footprint
cav_x = pcb_len + 2*clearance;
cav_y = pcb_wid + 2*clearance;
out_x = cav_x + 2*wall;
out_y = cav_y + 2*wall;

// Vertical stack (z = 0 at floor bottom, case sits flat on desk):
cavity_below_pcb   = pin_plastic_height + dupont_housing_h + 1.0;  // 17.5 mm
// Above PCB: accommodate LCD (2.2 mm) AND stackable pin-header tips that
// protrude upward (~5 mm typical for "long-pin" headers). Set by the
// TALLER of the two features.
cavity_above_pcb   = 5.5;
total_h            = floor_thk + cavity_below_pcb
                   + pcb_thk + cavity_above_pcb;                    // ≈ 24.4

// Z planes
z_floor_top   = floor_thk;
z_pcb_bottom  = z_floor_top + cavity_below_pcb;
z_pcb_top     = z_pcb_bottom + pcb_thk;
z_rim         = total_h;

/* [PCB corner-support tabs (chamfered wedges)] */
tab_arm_x = 5.0;
tab_arm_y = 4.0;
tab_depth = 1.5;
tab_thk   = 1.4;

/* [Wire-bundle exit slot — LEFT short wall (opposite USB-C)] */
wire_exit_w         = 14.0;   // width (Y) of the slot
wire_exit_h         = 7.0;    // height (Z) of the slot
wire_exit_z_center  = (z_floor_top + z_pcb_bottom) / 2;  // mid-way in the below-PCB cavity

/* [USB-C] */
// Type-C jack body ≈ 8.94 × 3.26 mm. Widen for thicker cable overmolds.
usbc_w = 9.5;
usbc_h = 4.0;
usbc_z_center = z_pcb_bottom + pcb_thk/2;

/* [microSD slot — DISABLED] */
// The SD socket is a push-push type mounted on the BACK of the PCB
// (component side), with the card inserted from UNDER the board toward the
// USB-C end. It is fully internal — no external cutout is needed.
sd_enable = 0;
sd_w            = 13.0;
sd_h            = 2.5;
sd_z_above_usbc = 5.5;

/* [BOOT / RST buttons — open-topped slot + outer press membrane] */
// The PCB tact-switches protrude ~1.5 mm sideways from the board edge —
// more than the 0.4 mm PCB-to-wall clearance, so the wall must give way
// in the button zone. Design:
//   1. A through-slot cut fully through the wall at the button X position,
//      open all the way to the top rim. This lets the switch ride down
//      through the slot during PCB insertion (closed-top = PCB stuck).
//   2. A thin press-membrane glued back INTO the outer face of the slot,
//      attached only at its bottom edge (living-hinge cantilever). Outer
//      face of the membrane is flush with the outer wall; inner face sits
//      ~1.0 mm deep, right in front of the switch actuator. Pressing the
//      membrane from outside flexes it inward and actuates the switch.
btn_enable           = 1;
btn_inset_from_usbc  = 8.0;    // X from USB-C inner wall face to slot center
                               // (shifted 2 mm toward board center)
// Slot center is placed ~1 mm below the switch actuator so the switch ends
// up near the TOP of the slot → closer to the free (most-deflecting) end of
// the bottom-hinged membrane, which makes the press easier.
btn_z_center         = z_pcb_top - 0.15;
btn_slot_w           = 6.0;    // slot width along wall (X)
btn_slot_h           = 4.5;    // slot height (Z) — enough for switch body clearance

// Membrane geometry (the press surface)
btn_membr_t      = 1.3;    // membrane thickness (7 × 0.2 mm layers)
btn_membr_inset  = 0.0;    // flush with outer wall perimeter (no recess).
btn_membr_side_g = 0.5;    // visible gap between membrane and slot side edges
                           // (those are the slits that let it flex)

/* [Inner groove along each long wall — clearance for pin-header bodies] */
// The through-hole pin-header plastic body extends ~2.5 mm inward from the
// PCB long edges. These grooves widen the cavity in the pin-row X-range so
// that the PCB slides straight down without catching, even under FDM
// tolerance. Groove depth eats into the wall slightly (wall remains 0.8 mm).
groove_enable = 1;
groove_depth  = 1.0;       // inward into the wall
groove_x_margin = 2.5;     // keep clear of corner tabs by this much per side

/* [Top lead-in chamfer] */
// Chamfer the top inner edge of the cavity so the PCB self-aligns as it
// drops in. Depth and angle below.
leadin_depth  = 1.5;       // vertical depth of chamfer

/* [Optional ext. connector on left short wall] */
ext_conn_enable = 0;
ext_conn_w      = 8.0;
ext_conn_h      = 8.0;
ext_conn_z_ctr  = z_pcb_bottom + pcb_thk/2;

/* [M2 screws] */
m2_pilot_d  = 1.7;
m2_clear_d  = 2.4;
m2_head_d   = 4.0;
m2_head_rcs = 1.5;
pilot_depth = 6.0;

// Screw-boss centers (top-view), at the 4 internal corners of the cavity wall
function boss_centers() = [
    [wall + corner_r - 0.5,            wall + corner_r - 0.5          ],
    [out_x - wall - corner_r + 0.5,    wall + corner_r - 0.5          ],
    [wall + corner_r - 0.5,            out_y - wall - corner_r + 0.5  ],
    [out_x - wall - corner_r + 0.5,    out_y - wall - corner_r + 0.5  ],
];

$fn = 64;

// ─── helpers ──────────────────────────────────────────────────────────────

module rounded_box(x, y, z, r) {
    hull() for (dx = [r, x-r], dy = [r, y-r])
        translate([dx, dy, 0]) cylinder(h=z, r=r);
}

module rounded_rect_2d(x, y, r) {
    hull() for (dx = [r, x-r], dy = [r, y-r])
        translate([dx, dy]) circle(r=r);
}

// ─── tray ─────────────────────────────────────────────────────────────────

module tray() {
    difference() {
        // ── solid outer body (flat-bottom box) ──
        rounded_box(out_x, out_y, total_h, corner_r);

        // ── cavity: carve interior from floor-top upward to open rim ──
        translate([wall, wall, z_floor_top])
            rounded_box(cav_x, cav_y, z_rim - z_floor_top + 0.1,
                        corner_r - wall);

        // ── wire-bundle exit slot on LEFT short wall ──
        translate([-0.1,
                   out_y/2 - wire_exit_w/2,
                   wire_exit_z_center - wire_exit_h/2])
            cube([wall + 0.2, wire_exit_w, wire_exit_h]);

        // ── USB-C cutout (right short wall) ──
        translate([out_x - wall - 0.1,
                   out_y/2 - usbc_w/2,
                   usbc_z_center - usbc_h/2])
            cube([wall + 0.2, usbc_w, usbc_h]);

        // ── microSD card slot (right short wall, above USB-C) ──
        if (sd_enable) {
            translate([out_x - wall - 0.1,
                       out_y/2 - sd_w/2,
                       usbc_z_center + sd_z_above_usbc - sd_h/2])
                cube([wall + 0.2, sd_w, sd_h]);
        }

        // ── BOOT & RST open-topped slots (through-hole, open to rim) ──
        // Slot spans from (btn_z_center - btn_slot_h/2) upward to just above
        // the top rim, so the switch actuator slides in from the top as the
        // PCB descends and ends up aligned with the slot at final seating.
        if (btn_enable) {
            for (sy = [0, 1]) {
                btn_x = out_x - wall - btn_inset_from_usbc;
                z_low  = btn_z_center - btn_slot_h/2;
                z_high = z_rim + 0.2;                   // open to top rim
                translate([btn_x - btn_slot_w/2,
                           (sy == 0) ? -0.1 : out_y - wall - 0.1,
                           z_low])
                    cube([btn_slot_w, wall + 0.2, z_high - z_low]);
            }
        }

        // ── Inner grooves for pin-header body clearance on long walls ──
        if (groove_enable) {
            groove_x1 = groove_x_margin;
            groove_x2 = out_x - groove_x_margin;
            // Groove extends from just above PCB-bottom level up to the top
            // rim, so the pin-header body slides in freely.
            gz1 = z_pcb_bottom - 1.0;
            gz2 = z_rim + 0.1;
            // -Y wall groove (carved into the inner face of -Y wall)
            translate([groove_x1, wall - 0.01, gz1])
                cube([groove_x2 - groove_x1, groove_depth, gz2 - gz1]);
            // +Y wall groove
            translate([groove_x1, out_y - wall - groove_depth + 0.01, gz1])
                cube([groove_x2 - groove_x1, groove_depth, gz2 - gz1]);
        }

        // ── Top lead-in chamfer (inside cavity top edge) ──
        // Carved by hull between the cavity at the top (enlarged) and at
        // leadin_depth below (normal size). Uses minkowski-like approach
        // via a cone subtraction implemented as a linear-extrude scale.
        translate([0, 0, z_rim - leadin_depth])
            hull() {
                translate([wall - leadin_depth, wall - leadin_depth, 0])
                    rounded_box(cav_x + 2*leadin_depth,
                                cav_y + 2*leadin_depth,
                                0.01,
                                corner_r - wall + leadin_depth);
                translate([wall, wall, leadin_depth])
                    rounded_box(cav_x, cav_y, 0.01, corner_r - wall);
            }

        // ── Optional external connector (left short wall) ──
        if (ext_conn_enable)
            translate([-0.1,
                       out_y/2 - ext_conn_w/2,
                       ext_conn_z_ctr - ext_conn_h/2])
                cube([wall + 0.2, ext_conn_w, ext_conn_h]);

        // ── Screw pilot holes in the 4 internal corner bosses ──
        for (p = boss_centers())
            translate([p[0], p[1], z_rim - pilot_depth + 0.01])
                cylinder(h = pilot_depth + 0.1, d = m2_pilot_d);
    }

    // ── Press membrane inside each button slot (outer-aligned cantilever) ──
    // Added AFTER the through-slot cut so the result is: wall has a hole,
    // with a thin flap covering most of the hole from outside, attached
    // only along its bottom edge. User presses this flap to actuate the
    // PCB switch; side/top gaps are the visible "slits" that let it flex.
    // The membrane extends 0.6 mm below the slot's z_low into solid wall
    // material so it welds to the wall there — without this overlap it
    // would just touch at a plane and come out as a floating flap.
    if (btn_enable) {
        for (sy = [0, 1]) {
            btn_x = out_x - wall - btn_inset_from_usbc;
            z_low  = btn_z_center - btn_slot_h/2;
            m_anchor_ov = 0.6;
            // Membrane footprint: slot width minus side gaps (those form the
            // visible slits that let it flex), height from anchor overlap at
            // bottom all the way up to the top rim so the slot is fully
            // covered on the outer face of the case.
            m_x    = btn_x - btn_slot_w/2 + btn_membr_side_g;
            m_w    = btn_slot_w - 2*btn_membr_side_g;
            m_z1   = z_low - m_anchor_ov;
            m_z2   = z_rim;
            m_y    = (sy == 0) ? btn_membr_inset
                                : out_y - btn_membr_inset - btn_membr_t;
            translate([m_x, m_y, m_z1])
                cube([m_w, btn_membr_t, m_z2 - m_z1]);
        }
    }

    // ── Chamfered corner tabs that support the PCB from below ──
    for (cx = [0, 1], cy = [0, 1]) {
        // X-arm: along the long wall at this corner
        x0 = (cx == 0) ? wall : out_x - wall - tab_arm_x;
        y_wall = (cy == 0) ? wall : out_y - wall;
        y_dir  = (cy == 0) ? 1 : -1;
        hull() {
            translate([x0,
                       y_wall + (y_dir > 0 ? 0 : -tab_depth),
                       z_pcb_bottom - 0.01])
                cube([tab_arm_x, tab_depth, 0.01]);
            translate([x0,
                       y_wall + (y_dir > 0 ? 0 : -0.05),
                       z_pcb_bottom - tab_thk])
                cube([tab_arm_x, 0.05, 0.01]);
        }
        // Y-arm: along the short wall at this corner
        y0 = (cy == 0) ? wall : out_y - wall - tab_arm_y;
        x_wall = (cx == 0) ? wall : out_x - wall;
        x_dir  = (cx == 0) ? 1 : -1;
        hull() {
            translate([x_wall + (x_dir > 0 ? 0 : -tab_depth),
                       y0,
                       z_pcb_bottom - 0.01])
                cube([tab_depth, tab_arm_y, 0.01]);
            translate([x_wall + (x_dir > 0 ? 0 : -0.05),
                       y0,
                       z_pcb_bottom - tab_thk])
                cube([0.05, tab_arm_y, 0.01]);
        }
    }
}

// ─── lid ──────────────────────────────────────────────────────────────────

module lid() {
    difference() {
        rounded_box(out_x, out_y, lid_thk, corner_r);

        // LCD window
        translate([out_x/2 - lcd_active_long/2 + lcd_x_offset,
                   out_y/2 - lcd_active_short/2,
                   -0.1])
            cube([lcd_active_long, lcd_active_short, lid_thk + 0.2]);

        // Screw through-holes + countersinks
        for (p = boss_centers()) {
            translate([p[0], p[1], -0.1])
                cylinder(h = lid_thk + 0.2, d = m2_clear_d);
            translate([p[0], p[1], -0.01])
                cylinder(h = m2_head_rcs, d1 = m2_head_d, d2 = m2_clear_d);
        }
    }
}

// ─── layout ───────────────────────────────────────────────────────────────

if (part == "tray")     tray();
else if (part == "lid") lid();
else { tray(); translate([out_x + 8, 0, 0]) lid(); }
