# Cross-mode Port B test: Servo (PWM) -> Battery (I2C) -> Motor (PWM).
# Verifies no leftover ownership prevents the next mode from acquiring.
$IP = "192.168.32.51"

function Tap($x, $y, $what) {
    Write-Host "  tap $what @ ($x,$y)"
    curl.exe --max-time 5 -sS -X POST "http://$IP/api/sys/ui/tap?x=$x&y=$y" 2>&1 | Out-Null
    Start-Sleep -Seconds 2
}
function PortStatus() {
    $r = curl.exe --max-time 3 -sS "http://$IP/api/port/status" | ConvertFrom-Json
    "$($r.mode_name)/$(if ($r.owner) { $r.owner } else { '<none>' })"
}

Write-Host "boot Port B: $(PortStatus)"

Write-Host "`n--- 1. Servo Start ---"
Tap 80 110 "Servo tile"
Tap 160 340 "Start"
Write-Host "  port: $(PortStatus)  (expect PWM/servo)"

Write-Host "`n--- 2. Servo Stop, back to home ---"
Tap 160 340 "Stop"
Write-Host "  port: $(PortStatus)  (expect IDLE)"
Tap 42 52 "Home"

Write-Host "`n--- 3. Battery (auto-acquires I2C) ---"
Tap 80 219 "Battery tile"
Write-Host "  port: $(PortStatus)  (expect I2C/battery)"
Tap 42 52 "Home"

Write-Host "`n--- 4. Motor Arm (must release I2C, acquire PWM) ---"
Tap 234 110 "Motor tile"
Tap 160 340 "Arm"
Write-Host "  port: $(PortStatus)  (expect PWM/motor)"

Write-Host "`n--- 5. Motor Disarm ---"
Tap 160 340 "Disarm"
Write-Host "  port: $(PortStatus)  (expect IDLE)"
Tap 42 52 "Home"

Write-Host "`n=== final log tail ==="
curl.exe --max-time 5 -sS "http://$IP/api/sys/log" 2>&1 | Select-Object -Last 30
