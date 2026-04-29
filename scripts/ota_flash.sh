#!/usr/bin/env bash
# Quick OTA flash for any env. Build, POST .bin to /api/ota/upload, then
# poll the board until it comes back up on the new firmware.
#
# Usage:
#   scripts/ota_flash.sh <env> [ip]
#
# Examples:
#   scripts/ota_flash.sh esp32s3                 # use saved IP for this env
#   scripts/ota_flash.sh wt32_sc01_plus 192.168.32.51
#   scripts/ota_flash.sh wt32_sc01_plus           # uses .board_ip.wt32_sc01_plus
#
# Per-env IP is cached under scripts/.board_ip.<env>.
#
# Why we tolerate curl exit 56 ("Recv failure: Connection was reset"):
# the OTA endpoint commits the flash *before* sending its 200 OK response,
# then spawns a task that disconnects WiFi + reboots ~1.5 s later. On the
# wire, curl often sees the WiFi.disconnect close the TCP connection
# before the response finishes draining its kernel buffer -- harmless,
# the firmware is already written and verified. We confirm success by
# probing /api/sys/mem after the reboot and checking the board comes
# back up.

set -euo pipefail
cd "$(dirname "$0")/.."

ENV="${1:-}"
if [ -z "$ENV" ]; then
    echo "Usage: $0 <env> [ip]" >&2
    echo "  env: esp32s3 | wt32_sc01_plus | ..." >&2
    exit 2
fi

IP_FILE="scripts/.board_ip.$ENV"
IP="${2:-}"
if [ -z "$IP" ] && [ -f "$IP_FILE" ]; then IP="$(cat "$IP_FILE")"; fi
if [ -z "$IP" ]; then
    echo "Usage: $0 $ENV <ip>   (saved to $IP_FILE for future runs)" >&2
    exit 2
fi
echo "$IP" > "$IP_FILE"

BIN=".pio/build/$ENV/firmware.bin"
if [ ! -f "$BIN" ] || [ "src" -nt "$BIN" ] || [ "include" -nt "$BIN" ] || [ "platformio.ini" -nt "$BIN" ]; then
    echo "[ota] Building $ENV..."
    pio run -e "$ENV"
fi

SIZE=$(wc -c < "$BIN")
echo "[ota] Uploading $BIN ($SIZE bytes) -> http://$IP/api/ota/upload"

# Tolerate curl's 56 (connection reset by peer mid-response) -- the
# expected outcome of the OTA endpoint's deauth+reboot dance.
set +e
curl --max-time 240 -sS -w "\n[ota] http_code=%{http_code} time=%{time_total}s\n" \
     -F "firmware=@$BIN;type=application/octet-stream" \
     "http://$IP/api/ota/upload"
CURL_RC=$?
set -e

if [ $CURL_RC -ne 0 ] && [ $CURL_RC -ne 56 ]; then
    echo "[ota] curl failed with rc=$CURL_RC (not the expected 0 or 56)" >&2
    exit 1
fi

# Confirm success by polling for the rebooted board. Reboot takes ~5 s
# (1.5 s deauth grace + reset + reboot + WiFi STA reconnect). Give it
# up to 60 s.
echo "[ota] Waiting for board to reboot and respond..."
for i in $(seq 1 60); do
    sleep 1
    if curl --max-time 2 -sS "http://$IP/api/sys/mem" >/dev/null 2>&1; then
        echo "[ota] Board responded after ~${i}s on the new firmware."
        exit 0
    fi
done
echo "[ota] Board did not come back after 60 s -- check the screen / serial." >&2
exit 1
