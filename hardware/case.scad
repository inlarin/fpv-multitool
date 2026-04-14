// ESP32-S3-LCD-1.47B enclosure — parametric
// Frame-on-legs case: pins soldered DOWN (5.5 mm tails), Dupont access from below.
//
// Units: millimeters. Render: F6, File → Export → Export as STL.
//
// ARCHITECTURE:
//   - 4 tall corner legs raise the PCB ~21 mm above the desk, so pin tails
//     (5.5 mm) + Dupont housings (~14 mm) fit underneath freely.
//   - Between the legs the sides are fully open → wires exit in any direction.
//   - Above the PCB: a short frame hugs the board, LCD faces up.
//   - Snap-on lid with LCD window and holes for BOOT / RST buttons.
//   - Optional cutout on the left short end for an external connector
//     (XT30 / JST / etc.) — disabled by default.
//
// VERIFY ON REAL BOARD BEFORE PRINTING:
//   - Pin-tail length (default 5.5 mm, per user)
//   - LCD module thickness above PCB (default 2.2 mm)
//   - USB-C cutout vs. your cable's overmold
//   - BOOT / RST button positions on lid (test by eye against the board)

/* [Render] */
part = "both";  // [tray, lid, both]

/* [PCB (Waveshare ESP32-S3-LCD-1.47B)] */
pcb_len       = 36.37;
pcb_wid       = 20.32;
pcb_thk       = 1.60;

// M2 mounting hole pattern (informational — not used in frame design)
hole_pattern_x = 32.43;
hole_pattern_y = 15.92;

/* [Pin headers — soldered pointing DOWN for Dupont] */
// User-measured projection of the soldered pin header below PCB:
//   total 9.5 mm below PCB bottom, of which
//   first 2.5 mm is the black plastic header body,
//   remaining 7.0 mm is exposed metal pin (where Dupont contacts grip).
pin_total_below_pcb = 9.5;
pin_plastic_height  = 2.5;
pin_metal_exposed   = pin_total_below_pcb - pin_plastic_height;  // = 7.0
// Dupont female housing seats against the plastic body and hangs below:
dupont_housing_h   = 14.0;
dupont_margin      = 2.0;   // gap between bottom of Dupont and desk

/* [LCD — 1.47" module on top face] */
lcd_active_x   = 30.72;
lcd_active_y   = 25.04;
lcd_module_thk = 2.20;

/* [Enclosure geometry] */
wall      = 1.80;
lid_thk   = 1.60;
clearance = 0.25;    // PCB ↔ inner wall gap
corner_r  = 2.00;

// Cavity (inside the upper frame)
cav_x = pcb_len + 2*clearance;
cav_y = pcb_wid + 2*clearance;
out_x = cav_x + 2*wall;
out_y = cav_y + 2*wall;

// Corner leg cross-section (how thick the corner pillars are)
leg_w = wall + 2.5;  // outer wall + inward shelf that supports PCB

// Under-PCB clearance height (legs):
// Dupont top butts against pin-header plastic, not against bare pin tip,
// so clearance is measured from PCB bottom → plastic → housing → margin.
leg_h = pin_plastic_height + dupont_housing_h + dupont_margin;  // = 18.5 mm

// Shelf thickness under PCB (formed by the step from "solid corner" to cavity)
shelf_thk = 1.5;

// Upper-frame height above PCB
headroom_above_lcd = 0.6;
frame_h_above_pcb  = pcb_thk + lcd_module_thk + headroom_above_lcd;

// Z reference planes (desk at z=0)
z_shelf_top   = leg_h;               // PCB bottom sits here
z_pcb_top     = z_shelf_top + pcb_thk;
total_h       = leg_h + shelf_thk + frame_h_above_pcb;
//  (the shelf is below PCB by shelf_thk — we subtract it from the step)

/* [USB-C cutout — right short end] */
usbc_w = 10.0;
usbc_h = 5.0;
usbc_z_center = z_shelf_top + pcb_thk/2;  // cable axis at mid-PCB thickness

/* [Optional external-connector cutout on LEFT short end] */
// Set ext_conn_enable=1 and tune size/position for XT30, JST, screw terminal, etc.
ext_conn_enable = 0;
ext_conn_w      = 8.0;
ext_conn_h      = 8.0;
ext_conn_z_ctr  = leg_h - 6.0;    // lowered into the leg zone (open face)

/* [Button access holes on lid — near USB-C end (right short side)] */
// Waveshare board: BOOT top-right corner, RST near USB-C on right edge.
// Holes pierce the lid top plate; use pointed tool or pin.
boot_enable = 1;
rst_enable  = 1;
btn_dia     = 3.0;
btn_inset_x = 4.5;    // from right short edge
boot_y_off  = 6.0;    // from lid center along short axis (+Y)
rst_y_off   = -6.0;   // (−Y)

/* [Snap fit] */
snap_enable = 1;
snap_h = 1.0;
snap_w = 4.0;
snap_t = 0.6;

$fn = 56;

// ─── helpers ──────────────────────────────────────────────────────────────

module rounded_box(x, y, z, r) {
    hull() for (dx = [r, x-r], dy = [r, y-r])
        translate([dx, dy, 0]) cylinder(h=z, r=r);
}

// ─── tray: 4 corner legs + upper frame with inward shelf ─────────────────

module tray() {
    difference() {
        // Solid outer block (legs + frame as one piece)
        rounded_box(out_x, out_y, total_h, corner_r);

        // --- carve out the open leg zone (everything between 4 corners) ---
        // Long-side openings (cut through Y)
        translate([leg_w, -0.1, -0.1])
            cube([out_x - 2*leg_w, out_y + 0.2, leg_h + 0.01]);
        // Short-side openings (cut through X)
        translate([-0.1, leg_w, -0.1])
            cube([out_x + 0.2, out_y - 2*leg_w, leg_h + 0.01]);

        // --- carve PCB cavity full-height (no floor) ---
        // Wires/pins pass freely from z=0 up to PCB bottom and beyond.
        translate([wall, wall, -0.1])
            rounded_box(cav_x, cav_y, total_h + 0.2, corner_r - wall);

        // USB-C cutout (right short end)
        translate([out_x - wall - 0.1,
                   out_y/2 - usbc_w/2,
                   usbc_z_center - usbc_h/2])
            cube([wall + 0.2, usbc_w, usbc_h]);

        // Optional external connector cutout (left short end)
        if (ext_conn_enable)
            translate([-0.1,
                       out_y/2 - ext_conn_w/2,
                       ext_conn_z_ctr - ext_conn_h/2])
                cube([wall + 0.2, ext_conn_w, ext_conn_h]);

        // Snap dimples (female) on long walls of the upper frame
        if (snap_enable) {
            z_snap = total_h - 1.5;
            for (ox = [out_x*0.28, out_x*0.72], sy = [0, 1]) {
                translate([ox - snap_w/2,
                           sy == 0 ? wall - 0.3 : out_y - wall - snap_t + 0.3,
                           z_snap - snap_h/2])
                    cube([snap_w, snap_t, snap_h]);
            }
        }
    }

    // ── PCB support: 4 L-shaped corner tabs protruding into the cavity ──
    // Tabs occupy only the corner regions, clear of pin-header rows.
    // Pin rows run along the long (X) edges in the middle of each long side,
    // so only the first/last ~6 mm along each long edge is pin-free at corners.
    shelf_arm_x = 5.0;   // tab length along long (X) edge
    shelf_arm_y = 4.0;   // tab length along short (Y) edge
    shelf_depth = 1.2;   // how far the tab sticks inward from the inner wall
    shelf_thk   = 1.5;   // vertical thickness of the tab
    z_tab_bot   = z_shelf_top - shelf_thk;

    for (cx = [0, 1], cy = [0, 1]) {
        // Piece along the long (X) wall — only at the very corner
        translate([cx == 0 ? wall : out_x - wall - shelf_arm_x,
                   cy == 0 ? wall : out_y - wall - shelf_depth,
                   z_tab_bot])
            cube([shelf_arm_x, shelf_depth, shelf_thk]);
        // Piece along the short (Y) wall
        translate([cx == 0 ? wall : out_x - wall - shelf_depth,
                   cy == 0 ? wall : out_y - wall - shelf_arm_y,
                   z_tab_bot])
            cube([shelf_depth, shelf_arm_y, shelf_thk]);
    }
}

// ─── lid ──────────────────────────────────────────────────────────────────

module lid() {
    skirt_h = 3.5;

    difference() {
        union() {
            rounded_box(out_x, out_y, lid_thk, corner_r);
            translate([0, 0, lid_thk])
                difference() {
                    rounded_box(out_x, out_y, skirt_h, corner_r);
                    translate([wall - 0.15, wall - 0.15, -0.1])
                        rounded_box(out_x - 2*wall + 0.3,
                                    out_y - 2*wall + 0.3,
                                    skirt_h + 0.2,
                                    corner_r - wall);
                }
        }

        // LCD window — centered on lid
        translate([out_x/2 - lcd_active_x/2,
                   out_y/2 - lcd_active_y/2,
                   -0.1])
            cube([lcd_active_x, lcd_active_y, lid_thk + 0.2]);

        // Button access holes — top (lid) surface, near USB-C short edge
        if (boot_enable)
            translate([out_x - btn_inset_x, out_y/2 + boot_y_off, -0.1])
                cylinder(h = lid_thk + 0.2, d = btn_dia);
        if (rst_enable)
            translate([out_x - btn_inset_x, out_y/2 + rst_y_off, -0.1])
                cylinder(h = lid_thk + 0.2, d = btn_dia);
    }

    // Snap bumps (male) on skirt inner walls
    if (snap_enable) {
        z_bump = lid_thk + skirt_h/2;
        for (ox = [out_x*0.28, out_x*0.72]) {
            translate([ox - snap_w/2, wall - 0.15, z_bump - snap_h/2])
                cube([snap_w, snap_t, snap_h]);
            translate([ox - snap_w/2, out_y - wall + 0.15 - snap_t,
                       z_bump - snap_h/2])
                cube([snap_w, snap_t, snap_h]);
        }
    }
}

// ─── layout ───────────────────────────────────────────────────────────────

if (part == "tray")      tray();
else if (part == "lid")  lid();
else { tray(); translate([out_x + 6, 0, 0]) lid(); }
