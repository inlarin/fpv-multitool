#!/usr/bin/env bash
# Full clean-install of vanilla ELRS 3.6.3 onto Bayck RC C3 Dual RX.
# Assumes RX is in DFU (BOOT held + power-applied) when script is launched.
#
# Order matters — write bootloader first so we don't have a half-good
# partition table booting a stale bootloader.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)/vanilla_elrs_3.6.3"
BOARD="${1:-http://192.168.32.50}"

flash_blob() {
    local name="$1" offset="$2" path="$DIR/$1"
    local size=$(wc -c < "$path" | tr -d ' ')
    echo
    echo "=== [$name] upload $size bytes ==="
    curl -sS --max-time 180 --fail \
         -F "firmware=@$path;type=application/octet-stream" \
         "$BOARD/api/flash/upload"
    echo
    echo "=== [$name] flash at $offset ==="
    curl -sS --max-time 10 -X POST -F "offset=$offset" "$BOARD/api/flash/start"
    echo
    # Poll until Done/Failed
    for i in $(seq 1 90); do
        stage=$(curl -sS --max-time 5 "$BOARD/api/sys/mem" \
            | python -c "import sys,json; print(json.load(sys.stdin).get('flash_stage',''))" 2>/dev/null)
        case "$stage" in
            Done|Failed) echo "[$name] stage=$stage (iter $i)"; break ;;
            "") [ "$i" -gt 3 ] && echo "[$name] stage cleared (done)" && break ;;
        esac
        printf "."
        sleep 2
    done
}

echo "=== STEP 1/4: bootloader @ 0x0000 ==="
flash_blob "bootloader.bin" "0x0"
echo "=== STEP 2/4: partitions @ 0x8000 ==="
flash_blob "partitions.bin" "0x8000"
echo "=== STEP 3/4: boot_app0 (OTADATA) @ 0xe000 ==="
flash_blob "boot_app0.bin" "0xe000"
echo "=== STEP 4/4: firmware (vanilla ELRS with bayck) @ 0x10000 ==="
flash_blob "firmware.bin" "0x10000"
echo
echo "=== All done. Power-cycle RX without BOOT. ==="
