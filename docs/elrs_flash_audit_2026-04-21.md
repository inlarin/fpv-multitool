# ELRS Flash Subsystem — Deep Audit (2026-04-21)

## Trigger
Last flash attempt (`b3l5pddyd`) completed in 28 s for a 1.24 MB image at
0x10000 (way under the ~60 s physical minimum at 115200 baud), RX MD5 did not
match local `firmware.bin`, then plate briefly stopped responding. User's read
of the situation: *"сломан весь функционал прошивки elrs и взаимодействия с
приемником"*. This document isolates the real bugs from the red herrings.

## Scope audited
- [src/bridge/esp_rom_flasher.cpp](../src/bridge/esp_rom_flasher.cpp) — ROM protocol
- [src/web/web_server.cpp](../src/web/web_server.cpp) — flash endpoints + main-loop dispatch
- [src/web/web_state.h](../src/web/web_state.h) — shared flash state
- Cross-check: esptool.py `loader.py`, esp-serial-flasher `esp_loader.c`

## Real bugs found (ordered by impact)

### B1 — `setRxBufferSize(4096)` called **after** `begin()` → buffer stays at 256 B
[esp_rom_flasher.cpp:247-248](../src/bridge/esp_rom_flasher.cpp#L247-L248):
```cpp
s_uart->begin(cfg.baud_rate, SERIAL_8N1, cfg.rx_pin, cfg.tx_pin);
s_uart->setRxBufferSize(4096);
```
Arduino-ESP32 allocates the UART RX ring during `begin()`. A later
`setRxBufferSize()` is **ignored on this core version** — buffer stays at the
default 256 B. At 115200 baud the ROM sync drain (7 extra 10-byte SYNC replies
= 70 B) fits, but any follow-up burst (e.g. a FLASH_DATA ACK delivered right
after another ROM frame the handler emits out-of-order) can overflow 256 B and
**drop bytes mid-frame**. Our `slipRead()` then times out and `sendCmd()` falls
into the "short response" branch below.

**Duplicated in**: `readFlash()` (line 405), `spiFlashMd5()` (line 483),
`runUserCode()` (line 560), `eraseRegion()` (line 367). All wrong order.

### B2 — `sendCmd()` returns `true` on truncated response
[esp_rom_flasher.cpp:138](../src/bridge/esp_rom_flasher.cpp#L138):
```cpp
if (resp_size < 2 || n < 8 + resp_size) return true; // some commands have no status
```
This is a **failure-masking bug**. If the RX buffer drops bytes (see B1) or the
ROM stalls mid-reply, `slipRead()` returns a short frame, and `sendCmd()`
reports success. Combined with B1 this is exactly the path that lets
FLASH_DATA blocks "succeed" while nothing actually lands in flash — the
MD5-mismatch symptom observed on `b3l5pddyd`.

Only SYNC legitimately has no status bytes, and SYNC is handled separately
(via the discard loop in `sync()`). Every other command sends `[…][status:2]`.
The correct fallback is to **return `false`** when the frame is too short.

### B3 — `flashBegin()` 90 s timeout is applied per **chunk**, not in addition to the already-armed erase
[esp_rom_flasher.cpp:198](../src/bridge/esp_rom_flasher.cpp#L198)

Not a bug by itself, but worth noting: with 5 × 256 KB chunks we issue 5
FLASH_BEGINs, each with its own erase. ESP32-C3 ROM erases ~40 KB/s, so a
256 KB chunk erase takes ~6 s. Five in a row = ~30 s, which matches the 28 s
b3l5pddyd duration — consistent with **the ROM only ever erasing, the
FLASH_DATA blocks silently returning via B2**.

### B4 — `PinPort::acquire("elrs_flash")` holds Port B through the **full** flash
[web_server.cpp:476-494](../src/web/web_server.cpp#L476-L494)

`executeFlash()` runs on the main loop (see [web_server.cpp:3642-3646](../src/web/web_server.cpp#L3642-L3646)) and holds the PinPort lock for ~30 s.
During that time every endpoint that needs Port B returns 409 — including
`/api/flash/dump/status` if the user tries to peek at progress, and
`/api/sys/mem` if any other consumer is trying to acquire Port B. The browser
card probably just looks dead for half a minute.

Worse: because `executeFlash()` runs on the main loop (not an xTask), it
blocks **all** main-loop work: CRSF loop, ESC telemetry, WebSocket
broadcasts, battery service actions. We've been interpreting `curl` timeouts
during long flashes as "RX died" when often the plate itself is just
unresponsive.

### B5 — `flash_request`/`in_progress` access is not locked
[web_server.cpp:3643](../src/web/web_server.cpp#L3643), [web_server.cpp:2098](../src/web/web_server.cpp#L2098)

The HTTP handler (AsyncTCP task) writes `flash_request = true`; the main loop
reads it. Both are plain `bool` on a dual-core SoC. In practice AsyncWebServer
runs on the same core as `loop()` most of the time, so no race has been
observed, but it's fragile.

### B6 — `executeFlash()` leaves `stage` and `progress_pct` frozen on early return
[web_server.cpp:451-470](../src/web/web_server.cpp#L451-L470)

Every early-return path (`gunzip failed`, `ELRS parse failed`, unknown format,
PinPort busy) sets `in_progress = false` and `lastResult` but leaves the
stage string from a previous flash visible. UI looks inconsistent; not a
functional bug.

### B7 — Last-block checksum covers 0xFF padding
[esp_rom_flasher.cpp:210-218](../src/bridge/esp_rom_flasher.cpp#L210-L218)

For the final partial block our checksum covers the full 4 KB including
0xFF padding. **This matches `esp-serial-flasher`'s behaviour** (which pads
and re-checksums the full block), so not a divergence. esptool.py does it
differently (`checksum(data_raw)` with no padding), but we follow the C
reference. Leave as-is.

### B8 — `spiSetParams()` `flash_size` argument
[esp_rom_flasher.cpp:262-264](../src/bridge/esp_rom_flasher.cpp#L262-L264)

We pass `max(write_extent, 4 MB)` rather than the actual chip size. For the
Bayck RC C3 Dual (4 MB) this always evaluates to 4 MB and works, but if we
ever see an 8 MB receiver (vanilla ELRS 3.6.3 on RP4TD, etc.) the flash-size
check inside ROM will clamp writes. Parameterise eventually.

### B9 — `spiFlashMd5` relies on a 128-byte response buffer
[esp_rom_flasher.cpp:511](../src/bridge/esp_rom_flasher.cpp#L511)

Response is at most 32 bytes of payload + status. Fine. Mentioning only
because if the buffer were sized off it'd silently truncate (same family as
B2). Currently OK.

## Non-bugs (checked and cleared)

- **FLASH_BEGIN 20-byte payload with encryption=0** — correct for ESP32-C3 ROM
  (the 5th word is mandatory, see esptool serial-protocol docs).
- **FLASH_END(0=reboot, 1=stay)** — matches the ESP32-C3 ROM convention
  (esptool `flash_finish()` does `int(not reboot)`).
- **Per-chunk FLASH_BEGIN / FLASH_END(stay) pattern** — valid; esp-serial-flasher
  does the same for large images.
- **SPI_ATTACH + SPI_SET_PARAMS once per session** — sufficient; ROM caches.
- **FLASH_WRITE_BLOCK_SIZE = 4096** — *not* the 1-KB-hard-limit our sub-agent
  suggested. esp-serial-flasher defaults to 0x0400 for classic ESP32 but
  happily uses 0x1000 (4 KB) on C3. Issue #860 they referenced is a stub-mode
  corner case. The 287-block cliff we saw on 1 KB blocks was **almost certainly
  caused by B1+B2 together, not a ROM block-size limit**.

## Probable root cause of the MD5 mismatch

B1 (undersized RX ring) + B2 (silent success on truncated responses) produce
exactly the observed symptom:

1. FLASH_BEGIN → erase issued (works — large fixed payload, no drops).
2. FLASH_DATA block 1 → ACK arrives, fits in 256 B ring → OK.
3. FLASH_DATA block N (where N depends on timing) → ROM's combined reply
   plus leftover SYNC/FLASH_BEGIN tail overflows the 256 B ring → slipRead
   truncates → sendCmd hits the "resp_size<2 || n<8+resp_size" path → returns
   `true` → we think block N landed when it didn't.
4. Repeat for the rest of the image. No blocks error; nothing lands; FLASH_END
   is skipped (stay_in_loader). We exit the flash function returning FLASH_OK
   but the chip still has its old contents where we thought we wrote.
5. Subsequent MD5 readback (via READ_FLASH_SLOW, same protocol, same buggy
   ring) yields a different digest — the RX never saw the new bytes.

The 28-second duration supports this: 5 × 256 KB erase @ ~40 KB/s is exactly
~32 s, with the FLASH_DATA blocks spending ~zero time because all the "acks"
were actually truncated tails read almost immediately.

## Recommended fix sequence (smallest possible, in order)

**P0 — two-line fix, high probability of unblocking everything**:
1. Swap `setRxBufferSize()` before `begin()` in all four functions in
   `esp_rom_flasher.cpp`.
2. Change [line 138](../src/bridge/esp_rom_flasher.cpp#L138) to
   `return false` when the frame is truncated.

**P1 — robustness**:
3. Move `executeFlash()` off the main loop onto a dedicated
   `xTaskCreate("elrs_flash", …, core=1)` — the dump path already does this
   ([web_server.cpp:2158](../src/web/web_server.cpp#L2158)).
4. Reset `flashState.stage = ""` on every early-return from executeFlash.
5. Wrap `flash_request` access in `WebState::Lock`.

**P2 — verification**:
6. After each flash, automatically call `spiFlashMd5()` and compare against
   an MD5 we computed locally before sending. If mismatch, mark the flash
   FAILED instead of OK regardless of what FLASH_DATA "said".
7. Log every sendCmd result explicitly (opcode + status + resp_size) to
   Serial at debug level so the next failure leaves a trail.

## Files to touch for P0

| File | Change |
|---|---|
| `src/bridge/esp_rom_flasher.cpp:247-248,367,405,482,559-560` | Reorder `setRxBufferSize` before `begin` |
| `src/bridge/esp_rom_flasher.cpp:138` | Change `return true` to `return false` on truncated frame |

Both changes together are <20 LOC. Probability this alone gets vanilla ELRS
flashed clean is high enough that I'd test before going further.
