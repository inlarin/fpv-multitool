# Build, OTA, then verify Servo START actually engages PWM on a fresh boot
# (no prior Battery screen entry to clear the I2C grab). Tests the
# boot-time Port B fix.

Write-Host "=== Build ==="
pio run -e wt32_sc01_plus 2>&1 | Select-String -Pattern "SUCCESS|FAILED|error:" | Select-Object -First 5

Write-Host "`n=== OTA ==="
$bin = ".pio/build/wt32_sc01_plus/firmware.bin"
curl.exe --max-time 60 -sS -F "fw=@$bin" http://192.168.32.51/api/ota/upload 2>&1 | Select-Object -Last 3

Write-Host "`n=== Wait for board ==="
$start = Get-Date
$ok = $false
while (((Get-Date) - $start).TotalSeconds -lt 60) {
    Start-Sleep -Seconds 2
    $r = curl.exe --max-time 3 -sS http://192.168.32.51/api/health 2>$null
    if ($r) { $ok = $true; break }
}
if (-not $ok) { Write-Host "FAIL: board didn't come back"; exit 1 }
Write-Host "alive"

Write-Host "`n=== Port B state at boot (should be IDLE) ==="
curl.exe --max-time 3 -sS http://192.168.32.51/api/port/status

Write-Host "`n=== Tap Servo, then Start ==="
curl.exe --max-time 3 -sS -X POST "http://192.168.32.51/api/sys/ui/tap?x=80&y=110" 2>&1 | Out-Null
Start-Sleep -Seconds 2
curl.exe --max-time 3 -sS -X POST "http://192.168.32.51/api/sys/ui/tap?x=160&y=340" 2>&1 | Out-Null
Start-Sleep -Seconds 2

Write-Host "`n=== Log tail (looking for [servo] START) ==="
curl.exe --max-time 3 -sS http://192.168.32.51/api/sys/log 2>&1 | Select-Object -Last 15

Write-Host "`n=== Port B state after Servo START ==="
curl.exe --max-time 3 -sS http://192.168.32.51/api/port/status
