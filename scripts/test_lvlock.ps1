# Verify screenshot endpoint works under the new lvLock and back-button
# nav still doesn't crash. Hammers the screenshot endpoint several times
# while clicking around -- exercises the cross-task lock.
$IP = "192.168.32.51"

Write-Host "=== Build + OTA ==="
pio run -e wt32_sc01_plus 2>&1 | Select-String -Pattern "SUCCESS|FAILED|error:" | Select-Object -First 5
$bin = ".pio/build/wt32_sc01_plus/firmware.bin"
curl.exe --max-time 60 -sS -F "fw=@$bin" http://$IP/api/ota/upload 2>&1 | Select-Object -Last 3

Write-Host "`n=== Wait for board ==="
$start = Get-Date
$ok = $false
while (((Get-Date) - $start).TotalSeconds -lt 60) {
    Start-Sleep -Seconds 2
    $r = curl.exe --max-time 3 -sS http://$IP/api/health 2>$null
    if ($r) { $ok = $true; break }
}
if (-not $ok) { Write-Host "FAIL: no board"; exit 1 }
Write-Host "alive"

Write-Host "`n=== Hammer screenshot + nav ==="
for ($i = 1; $i -le 3; $i++) {
    Write-Host "iteration $i"
    curl.exe --max-time 5 -sS -X POST "http://$IP/api/sys/ui/tap?x=80&y=110" 2>&1 | Out-Null
    Start-Sleep -Milliseconds 800
    curl.exe --max-time 15 -sS -o C:/tmp/lvlock-$i.bmp http://$IP/api/sys/screenshot.bmp 2>&1 | Out-Null
    if (Test-Path C:/tmp/lvlock-$i.bmp) {
        $sz = (Get-Item C:/tmp/lvlock-$i.bmp).Length
        Write-Host "  screenshot ${i}: $sz bytes"
    } else {
        Write-Host "  screenshot ${i}: FAIL"
    }
    curl.exe --max-time 5 -sS -X POST "http://$IP/api/sys/ui/tap?x=42&y=52" 2>&1 | Out-Null
    Start-Sleep -Milliseconds 500
}

$u = (curl.exe --max-time 3 -sS "http://$IP/api/health" | ConvertFrom-Json).uptime_s
Write-Host "`nuptime now: $u s (no reboot if monotonic)"

Write-Host "`n=== log tail ==="
curl.exe --max-time 3 -sS "http://$IP/api/sys/log" 2>&1 | Select-Object -Last 12
