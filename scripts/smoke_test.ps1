# Walk every section tile, press Back, confirm board never reboots.
$ErrorActionPreference = "Continue"
$IP = "192.168.32.51"

# Tile centers in the 2x4 home grid (W=320, H=480, status bar 24, padding 12,
# tile 142x97, gap 12). Springboard layout from board_app.cpp.
$tiles = @(
    @{ name = "Servo";    x = 80;  y = 110 },
    @{ name = "Motor";    x = 234; y = 110 },
    @{ name = "Battery";  x = 80;  y = 219 },
    @{ name = "ELRS RX";  x = 234; y = 219 },
    @{ name = "RC Sniff"; x = 80;  y = 328 },
    @{ name = "Catalog";  x = 234; y = 328 },
    @{ name = "Settings"; x = 80;  y = 437 }
)

$u0 = (curl.exe --max-time 5 -sS http://$IP/api/health | ConvertFrom-Json).uptime_s
Write-Host "[$u0 s] starting smoke test"

foreach ($t in $tiles) {
    $u_before = (curl.exe --max-time 5 -sS http://$IP/api/health | ConvertFrom-Json).uptime_s
    Write-Host "`n[$u_before s] -> tap $($t.name) at ($($t.x),$($t.y))"
    curl.exe --max-time 5 -sS -X POST "http://$IP/api/sys/ui/tap?x=$($t.x)&y=$($t.y)" 2>&1 | Out-Null
    Start-Sleep -Seconds 3

    $u_section = (curl.exe --max-time 5 -sS http://$IP/api/health 2>&1 | ConvertFrom-Json).uptime_s
    if ($null -eq $u_section -or $u_section -lt $u_before) {
        Write-Host "  *** FAIL: board crashed on entering $($t.name) (uptime $u_section < $u_before)" -ForegroundColor Red
        return
    }
    Write-Host "  [$u_section s] in section, tap Home"

    curl.exe --max-time 5 -sS -X POST "http://$IP/api/sys/ui/tap?x=42&y=52" 2>&1 | Out-Null
    Start-Sleep -Seconds 3

    $u_after = (curl.exe --max-time 5 -sS http://$IP/api/health 2>&1 | ConvertFrom-Json).uptime_s
    if ($null -eq $u_after -or $u_after -lt $u_section) {
        Write-Host "  *** FAIL: board crashed on Back from $($t.name) (uptime $u_after < $u_section)" -ForegroundColor Red
        return
    }
    Write-Host "  [$u_after s] back home OK"
}

Write-Host "`n*** PASS: all 7 sections enter+exit cleanly ***" -ForegroundColor Green

Write-Host "`n=== final log tail ==="
curl.exe --max-time 5 -sS http://$IP/api/sys/log 2>&1 | Select-Object -Last 25
