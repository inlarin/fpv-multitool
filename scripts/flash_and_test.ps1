# All-in-one: build, OTA flash, verify back-button doesn't crash.
# Designed to run as a single command so we only need one permission grant.
# Don't set ErrorActionPreference=Stop -- pio writes warnings to stderr
# (NativeCommandError) and PowerShell would abort the whole run.

Write-Host "=== Build ==="
pio run -e wt32_sc01_plus 2>&1 | Select-Object -Last 6

Write-Host "`n=== Pre-flash health ==="
$h = curl.exe --max-time 5 -sS http://192.168.32.51/api/health
Write-Host $h

Write-Host "`n=== OTA upload ==="
$bin = ".pio/build/wt32_sc01_plus/firmware.bin"
curl.exe --max-time 60 -sS -F "fw=@$bin" http://192.168.32.51/api/ota/upload
Write-Host ""

Write-Host "`n=== Wait for board to come back ==="
$start = Get-Date
$ok = $false
while (((Get-Date) - $start).TotalSeconds -lt 60) {
    Start-Sleep -Seconds 2
    $r = curl.exe --max-time 3 -sS http://192.168.32.51/api/health 2>$null
    if ($r) { $ok = $true; Write-Host "alive after $([int]((Get-Date)-$start).TotalSeconds)s: $r"; break }
}
if (-not $ok) { Write-Error "board didn't come back"; exit 1 }

Write-Host "`n=== Verify back-button fix: tap Servo, then Home, check uptime stays continuous ==="
$u1 = (curl.exe --max-time 3 -sS http://192.168.32.51/api/health | ConvertFrom-Json).uptime_s
Write-Host "uptime before: $u1"

curl.exe --max-time 5 -sS -X POST "http://192.168.32.51/api/sys/ui/tap?x=80&y=110" | Out-Null
Start-Sleep -Seconds 2
curl.exe --max-time 5 -sS -X POST "http://192.168.32.51/api/sys/ui/tap?x=42&y=52" | Out-Null
Start-Sleep -Seconds 5

$u2 = (curl.exe --max-time 3 -sS http://192.168.32.51/api/health | ConvertFrom-Json).uptime_s
Write-Host "uptime after enter+back+5s: $u2 (delta $($u2-$u1)s; reboot would reset to ~5)"

if ($u2 -gt $u1) {
    Write-Host "`n*** PASS: no reboot, back-button fix works ***" -ForegroundColor Green
} else {
    Write-Host "`n*** FAIL: uptime reset, board rebooted ***" -ForegroundColor Red
}

Write-Host "`n=== Last log lines ==="
curl.exe --max-time 3 -sS http://192.168.32.51/api/sys/log | Select-Object -Last 15
