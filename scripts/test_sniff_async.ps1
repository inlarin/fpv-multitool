# Verify RC Sniff auto-detect no longer blocks loopTask: status bar
# uptime should keep ticking while detection runs (1.5 s nominally).
$IP = "192.168.32.51"

# Force back to home in case some other section is open from prior tests.
curl.exe --max-time 5 -sS -X POST "http://$IP/api/sys/ui/tap?x=42&y=52" 2>&1 | Out-Null
Start-Sleep -Seconds 2

# Navigate to RC Sniff (col 0, row 2)
curl.exe --max-time 5 -sS -X POST "http://$IP/api/sys/ui/tap?x=80&y=328" 2>&1 | Out-Null
Start-Sleep -Seconds 2

$u_before = (curl.exe --max-time 3 -sS "http://$IP/api/health" | ConvertFrom-Json).uptime_s
$ms_before = [DateTimeOffset]::Now.ToUnixTimeMilliseconds()
Write-Host "[$u_before s] tapping Auto-detect"

# Tap Auto-detect button. With the state machine fix, this returns
# immediately. With the old blocking impl it would have been stuck for
# 1.5 s and status bar uptime would have stalled.
curl.exe --max-time 3 -sS -X POST "http://$IP/api/sys/ui/tap?x=160&y=255" 2>&1 | Out-Null

# Sample uptime mid-detection (~750 ms in)
Start-Sleep -Milliseconds 800
$u_mid = (curl.exe --max-time 3 -sS "http://$IP/api/health" | ConvertFrom-Json).uptime_s
$ms_mid = [DateTimeOffset]::Now.ToUnixTimeMilliseconds()
Write-Host "[$u_mid s] mid-detection (~800ms after click)"

Start-Sleep -Seconds 1
$u_after = (curl.exe --max-time 3 -sS "http://$IP/api/health" | ConvertFrom-Json).uptime_s
Write-Host "[$u_after s] post-detection"

if ($u_mid -gt $u_before) {
    Write-Host "*** PASS: uptime advanced during detection ($u_before -> $u_mid -> $u_after) ***" -ForegroundColor Green
} else {
    Write-Host "*** FAIL: uptime stuck = loopTask blocked ***" -ForegroundColor Red
}

Write-Host "`n=== sniff log ==="
curl.exe --max-time 3 -sS "http://$IP/api/sys/log" 2>&1 | Select-String -Pattern "sniff|indev"  | Select-Object -Last 15
