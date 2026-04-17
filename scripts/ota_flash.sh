#!/usr/bin/env bash
# Quick OTA flash: builds firmware, uploads via /api/ota to the board.
#
# Usage:  scripts/ota_flash.sh [ip]
# Board IP is read from scripts/.board_ip (created on first run) or passed as arg.

set -euo pipefail
cd "$(dirname "$0")/.."

IP_FILE="scripts/.board_ip"
IP="${1:-}"
if [ -z "$IP" ] && [ -f "$IP_FILE" ]; then IP="$(cat "$IP_FILE")"; fi
if [ -z "$IP" ]; then
    echo "Usage: $0 <board-ip>   (saved to $IP_FILE for future runs)" >&2
    exit 2
fi
echo "$IP" > "$IP_FILE"

BIN=".pio/build/esp32s3/firmware.bin"
if [ ! -f "$BIN" ] || [ "src" -nt "$BIN" ]; then
    echo "[ota] Building firmware…"
    pio run
fi

SIZE=$(wc -c < "$BIN")
echo "[ota] Uploading $BIN ($SIZE bytes) → http://$IP/api/ota"

curl --fail --max-time 120 -sS \
     -F "firmware=@$BIN;type=application/octet-stream" \
     "http://$IP/api/ota"
echo
echo "[ota] Done. Board is rebooting."
