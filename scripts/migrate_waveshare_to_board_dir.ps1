# One-shot migration: move all Waveshare-specific code from scattered
# locations under src/ into src/board/wsh_s3_lcd_147b/ and rewrite
# #include paths so both envs still compile.
#
# Run from project root:
#   powershell -ExecutionPolicy Bypass -File scripts/migrate_waveshare_to_board_dir.ps1
#
# Idempotent: re-running after partial completion picks up where it left
# off (skips files already in their target location).

$root = Resolve-Path "."
$dst  = "src/board/wsh_s3_lcd_147b"

Write-Host "=== 1. Create target dirs ==="
New-Item -ItemType Directory -Force -Path "$dst", "$dst/ui" | Out-Null

# (old, new) pairs. New paths are relative to repo root.
$moves = @(
    # Hardware abstraction layer at board root
    @('src/main.cpp',                          "$dst/main.cpp"),
    @('src/ui/display.cpp',                    "$dst/display.cpp"),
    @('src/ui/display.h',                      "$dst/display.h"),
    @('src/ui/button.cpp',                     "$dst/button.cpp"),
    @('src/ui/button.h',                       "$dst/button.h"),
    @('src/ui/status_led.cpp',                 "$dst/status_led.cpp"),
    @('src/ui/status_led.h',                   "$dst/status_led.h"),
    @('src/bridge/usb2ttl.cpp',                "$dst/usb2ttl.cpp"),
    @('src/bridge/usb2ttl.h',                  "$dst/usb2ttl.h"),
    # UI / screens layer under ui/
    @('src/ui/menu.cpp',                       "$dst/ui/menu.cpp"),
    @('src/ui/menu.h',                         "$dst/ui/menu.h"),
    @('src/web/wifi_app.cpp',                  "$dst/ui/wifi_app.cpp"),
    @('src/web/wifi_app.h',                    "$dst/ui/wifi_app.h"),
    @('src/crsf/crsf_tester.cpp',              "$dst/ui/crsf_tester.cpp"),
    @('src/crsf/crsf_tester.h',                "$dst/ui/crsf_tester.h"),
    @('src/motor/motor_tester.cpp',            "$dst/ui/motor_tester.cpp"),
    @('src/motor/motor_tester.h',              "$dst/ui/motor_tester.h"),
    @('src/servo/servo_tester.cpp',            "$dst/ui/servo_tester.cpp"),
    @('src/servo/servo_tester.h',              "$dst/ui/servo_tester.h"),
    @('src/battery/battery_ui.cpp',            "$dst/ui/battery_ui.cpp"),
    @('src/battery/battery_ui.h',              "$dst/ui/battery_ui.h"),
    @('src/battery/smbus_bridge_ui.cpp',       "$dst/ui/smbus_bridge_ui.cpp"),
    @('src/battery/smbus_bridge_ui.h',         "$dst/ui/smbus_bridge_ui.h")
)

Write-Host "`n=== 2. git mv all files ==="
foreach ($pair in $moves) {
    $old = $pair[0]; $new = $pair[1]
    if (Test-Path $new) {
        Write-Host "  SKIP (already moved): $new"
        continue
    }
    if (-not (Test-Path $old)) {
        Write-Host "  MISS (source gone, target gone): $old"
        continue
    }
    Write-Host "  $old -> $new"
    git mv $old $new
}

Write-Host "`n=== 3. Rewrite #include paths ==="
# Path translations applied to EVERY file under src/ (broad sweep is
# safe -- the tokens we replace are unique and not used by any shared
# code).
$replacements = @{
    '#include "ui/display.h"'                = '#include "board/wsh_s3_lcd_147b/display.h"'
    '#include "ui/button.h"'                 = '#include "board/wsh_s3_lcd_147b/button.h"'
    '#include "ui/status_led.h"'             = '#include "board/wsh_s3_lcd_147b/status_led.h"'
    '#include "ui/menu.h"'                   = '#include "board/wsh_s3_lcd_147b/ui/menu.h"'
    '#include "bridge/usb2ttl.h"'            = '#include "board/wsh_s3_lcd_147b/usb2ttl.h"'
    '#include "web/wifi_app.h"'              = '#include "board/wsh_s3_lcd_147b/ui/wifi_app.h"'
    '#include "crsf/crsf_tester.h"'          = '#include "board/wsh_s3_lcd_147b/ui/crsf_tester.h"'
    '#include "motor/motor_tester.h"'        = '#include "board/wsh_s3_lcd_147b/ui/motor_tester.h"'
    '#include "servo/servo_tester.h"'        = '#include "board/wsh_s3_lcd_147b/ui/servo_tester.h"'
    '#include "battery/battery_ui.h"'        = '#include "board/wsh_s3_lcd_147b/ui/battery_ui.h"'
    '#include "battery/smbus_bridge_ui.h"'   = '#include "board/wsh_s3_lcd_147b/ui/smbus_bridge_ui.h"'
    # Relative-from-bridge/ form used by old usb2ttl.cpp before move
    '#include "../ui/display.h"'             = '#include "board/wsh_s3_lcd_147b/display.h"'
    '#include "../ui/button.h"'              = '#include "board/wsh_s3_lcd_147b/button.h"'
    '#include "../ui/status_led.h"'          = '#include "board/wsh_s3_lcd_147b/status_led.h"'
}

# Sweep every .cpp/.h under src/.
$files = Get-ChildItem -Recurse -Path src -Include *.cpp, *.h -File
$total_changes = 0
foreach ($file in $files) {
    $content = Get-Content -Raw -LiteralPath $file.FullName
    if ($null -eq $content) { continue }
    $orig = $content
    foreach ($k in $replacements.Keys) {
        $content = $content.Replace($k, $replacements[$k])
    }
    if ($content -ne $orig) {
        # Count actual replacements made for diagnostics
        $delta = 0
        foreach ($k in $replacements.Keys) {
            $cnt_orig = ([regex]::Matches($orig,    [regex]::Escape($k))).Count
            $cnt_new  = ([regex]::Matches($content, [regex]::Escape($k))).Count
            $delta += ($cnt_orig - $cnt_new)
        }
        $rel = $file.FullName.Substring($root.Path.Length + 1)
        Write-Host "  $rel  ($delta replacements)"
        Set-Content -LiteralPath $file.FullName -Value $content -NoNewline -Encoding utf8
        $total_changes += $delta
    }
}
Write-Host "Total include rewrites: $total_changes"

Write-Host "`n=== 4. Drop now-empty source dirs ==="
foreach ($dir in @('src/ui', 'src/bridge')) {
    if (Test-Path $dir) {
        $rem = Get-ChildItem -Path $dir -File -ErrorAction SilentlyContinue
        if (-not $rem) {
            Write-Host "  rmdir $dir"
            Remove-Item -Recurse -Force $dir
        } else {
            Write-Host "  KEEP $dir (still has files):"
            $rem | ForEach-Object { Write-Host "    $($_.Name)" }
        }
    }
}

Write-Host "`n=== Done. Now: ==="
Write-Host "  1. Update platformio.ini build_src_filter for both envs"
Write-Host "  2. pio run -e esp32s3"
Write-Host "  3. pio run -e wt32_sc01_plus"
