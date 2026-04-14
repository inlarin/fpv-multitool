// ESP32-S3-LCD-1.47B enclosure — parametric
// Units: millimeters
// Render: F6, then File → Export → Export as STL
//
// VERIFY BEFORE PRINTING:
//   - Measure total stack height (USB-C bottom to LCD glass top) with calipers
//   - Confirm JST battery connector position (not accounted for below)
//   - Confirm USB-C cutout height matches your cable's overmold

/* [What to render] */
part = "both"; // [tray, lid, both]

/* [PCB] */
pcb_len       = 36.37;
pcb_wid       = 20.32;
pcb_thk       = 1.60;

// Mounting hole pattern (center-to-center)
hole_pattern_x = 32.43;   // pcb_len - 2*1.97
hole_pattern_y = 15.92;   // pcb_wid - 2*2.20
hole_dia       = 2.20;    // M2 clearance
standoff_dia   = 4.00;
standoff_h     = 2.00;    // PCB sits this high above tray floor

/* [LCD — on top face of PCB] */
lcd_active_x   = 30.72;   // long axis of 172x320 display
lcd_active_y   = 25.04;
lcd_module_thk = 2.20;    // FPC + panel + glass above PCB surface

/* [Enclosure] */
wall       = 1.60;
floor_thk  = 1.50;
lid_thk    = 1.50;
clearance  = 0.20;        // gap between PCB and cavity wall
corner_r   = 2.00;        // outer corner radius

// Internal cavity (bottom tray)
cav_x = pcb_len + 2*clearance;
cav_y = pcb_wid + 2*clearance;
cav_z = standoff_h + pcb_thk + lcd_module_thk + 0.5;  // headroom

// Outer dimensions
out_x = cav_x + 2*wall;
out_y = cav_y + 2*wall;

tray_h = floor_thk + cav_z;
lid_skirt_h = 3.0;

/* [USB-C cutout — right short end] */
usbc_w = 10.0;   // width of cutout
usbc_h = 4.0;    // height of cutout (allow overmold)
// Vertical position: cable axis at PCB top surface
usbc_z_center = floor_thk + standoff_h + pcb_thk/2;

/* [Buttons — BOOT (top-right corner) and RST (right end, near USB-C)] */
// Since both are near the right end, use two small access holes on the top
// face of the tray wall or on the lid side. Here: lid side cutouts.
btn_dia = 3.0;
btn_gap = 4.0;    // spacing between BOOT and RST
btn_inset_from_usb_end = 6.0;  // from right short wall, inward

/* [Pin-header access slots on long sides — optional, set to 0 to disable] */
pin_slot_len = 30.0;
pin_slot_h   = 2.5;
pin_slot_enable = 1;

/* [Snap fit] */
snap_enable = 1;
snap_h = 1.0;
snap_w = 4.0;
snap_t = 0.6;   // protrusion

$fn = 48;

// ─── helpers ──────────────────────────────────────────────────────────────

module rounded_box(x, y, z, r) {
    hull() {
        for (dx = [r, x-r], dy = [r, y-r])
            translate([dx, dy, 0]) cylinder(h=z, r=r);
    }
}

module mounting_holes_2d() {
    // pattern centered on cavity
    cx = out_x/2;
    cy = out_y/2;
    for (sx = [-1, 1], sy = [-1, 1])
        translate([cx + sx*hole_pattern_x/2, cy + sy*hole_pattern_y/2])
            children();
}

// ─── tray ─────────────────────────────────────────────────────────────────

module tray() {
    difference() {
        // outer shell
        rounded_box(out_x, out_y, tray_h, corner_r);

        // internal cavity
        translate([wall, wall, floor_thk])
            rounded_box(cav_x, cav_y, cav_z + 1, corner_r - wall);

        // USB-C cutout in right short wall
        translate([out_x - wall - 0.1,
                   out_y/2 - usbc_w/2,
                   usbc_z_center - usbc_h/2])
            cube([wall + 0.2, usbc_w, usbc_h]);

        // Pin-header slots on both long walls at PCB top-surface level
        if (pin_slot_enable) {
            z_pin = floor_thk + standoff_h + pcb_thk - 0.3;
            // front long wall (y = 0)
            translate([(out_x - pin_slot_len)/2, -0.1, z_pin])
                cube([pin_slot_len, wall + 0.2, pin_slot_h]);
            // back long wall
            translate([(out_x - pin_slot_len)/2, out_y - wall - 0.1, z_pin])
                cube([pin_slot_len, wall + 0.2, pin_slot_h]);
        }

        // snap dimples (female) on long walls to mate with lid bumps
        if (snap_enable) {
            z_snap = tray_h - snap_h/2 - 0.5;
            for (ox = [6, out_x - 6], sy = [0, 1]) {
                translate([ox - snap_w/2,
                           sy == 0 ? wall - 0.3 : out_y - wall - snap_t + 0.3,
                           z_snap - snap_h/2])
                    cube([snap_w, snap_t, snap_h]);
            }
        }
    }

    // standoffs inside cavity
    translate([0, 0, floor_thk])
    mounting_holes_2d()
        difference() {
            cylinder(h=standoff_h, d=standoff_dia);
            translate([0, 0, -0.1])
                cylinder(h=standoff_h + 0.2, d=hole_dia);
        }
}

// ─── lid ──────────────────────────────────────────────────────────────────

module lid() {
    skirt_out_x = out_x;
    skirt_out_y = out_y;
    skirt_in_x  = out_x - 2*wall + 0.3;   // slip fit over tray
    skirt_in_y  = out_y - 2*wall + 0.3;

    difference() {
        union() {
            // top plate
            rounded_box(out_x, out_y, lid_thk, corner_r);
            // skirt
            translate([0, 0, lid_thk])
                difference() {
                    rounded_box(skirt_out_x, skirt_out_y, lid_skirt_h, corner_r);
                    translate([wall - 0.15, wall - 0.15, -0.1])
                        rounded_box(skirt_in_x, skirt_in_y,
                                    lid_skirt_h + 0.2, corner_r - wall);
                }
        }

        // LCD window — centered
        translate([out_x/2 - lcd_active_x/2,
                   out_y/2 - lcd_active_y/2,
                   -0.1])
            cube([lcd_active_x, lcd_active_y, lid_thk + 0.2]);

        // Button access holes on short end (opposite USB-C = left side)
        // If your BOOT/RST are on the right near USB-C, change x to out_x - ...
        btn_y = out_y/2;
        btn_x_center = btn_inset_from_usb_end;
        translate([btn_x_center, btn_y - btn_gap/2, lid_thk/2])
            rotate([0, 90, 0])
                cylinder(h = wall + 1, d = btn_dia, center=true);
        translate([btn_x_center, btn_y + btn_gap/2, lid_thk/2])
            rotate([0, 90, 0])
                cylinder(h = wall + 1, d = btn_dia, center=true);
    }

    // Snap bumps (male) on skirt inner walls
    if (snap_enable) {
        z_bump = lid_thk + lid_skirt_h/2;
        for (ox = [6, out_x - 6]) {
            // front skirt
            translate([ox - snap_w/2, wall - 0.15, z_bump - snap_h/2])
                cube([snap_w, snap_t, snap_h]);
            // back skirt
            translate([ox - snap_w/2, out_y - wall + 0.15 - snap_t,
                       z_bump - snap_h/2])
                cube([snap_w, snap_t, snap_h]);
        }
    }
}

// ─── layout ───────────────────────────────────────────────────────────────

if (part == "tray") tray();
else if (part == "lid") lid();
else {
    tray();
    translate([out_x + 5, 0, 0]) lid();
}
