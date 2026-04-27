#include "esp_rom_flasher.h"
#include "esp_rom_stubs.h"

// Cross-TU debug ring (defined in routes_flash.cpp). Declared at global
// scope so it lives outside both ESPFlasher and RoutesFlash namespaces —
// otherwise the linker silently builds two unrelated symbols.
extern char *g_dfu_debug_buf;
extern size_t g_dfu_debug_len;

// ESP ROM Bootloader Protocol for ESP8266/ESP8285
// Reference: esptool.py, espressif/esp-serial-flasher
//
// SLIP framing:
//   0xC0 ... 0xC0
//   0xC0 → 0xDB 0xDC
//   0xDB → 0xDB 0xDD
//
// Command packet (after SLIP decode):
//   [0] direction: 0x00 (request)
//   [1] command
//   [2-3] size (LE uint16)
//   [4-7] checksum (LE uint32, XOR of data starting with 0xEF)
//   [8+] data
//
// Response packet:
//   [0] 0x01 (response)
//   [1] command (echo)
//   [2-3] size
//   [4-7] value (for READ_REG, etc.)
//   [8+] data + status(2 bytes: 0x00 = OK)

namespace ESPFlasher {

// ROM commands
static const uint8_t CMD_FLASH_BEGIN  = 0x02;
static const uint8_t CMD_FLASH_DATA   = 0x03;
static const uint8_t CMD_FLASH_END    = 0x04;
static const uint8_t CMD_MEM_BEGIN    = 0x05;  // alloc RAM region for stub
static const uint8_t CMD_MEM_END      = 0x06;  // jump to entry / commit
static const uint8_t CMD_MEM_DATA     = 0x07;  // RAM data block (stub upload)
static const uint8_t CMD_SYNC         = 0x08;
static const uint8_t CMD_READ_REG     = 0x0A;
static const uint8_t CMD_SPI_SET_PARAMS = 0x0B;  // declare geometry before READ_FLASH
static const uint8_t CMD_SPI_ATTACH   = 0x0D;
static const uint8_t CMD_READ_FLASH_SLOW = 0x0E;  // ROM native, 64 B/request
static const uint8_t CMD_CHANGE_BAUD  = 0x0F;
static const uint8_t CMD_READ_FLASH   = 0xD2;  // stub-only stream-read
static const uint8_t CMD_RUN_USER_CODE = 0xD3; // exit stub/ROM -> boot selected app
static const uint8_t CMD_SPI_FLASH_MD5 = 0x13; // fast verify: ROM computes MD5 over a region

// Parameters
// 4 KB per FLASH_DATA packet — aligns with SPI flash sector size so each
// ack corresponds to exactly one erase-unit commit. 1 KB blocks caused
// ROM to fall off a cliff at block 287 (the 288th 1-KB ack apparently
// hits an internal buffer boundary). 4 KB is what esptool's stub uses.
static const size_t FLASH_WRITE_BLOCK_SIZE = 4096;    // per FLASH_DATA packet
static const uint32_t DEFAULT_TIMEOUT_MS = 3000;
static const uint32_t SYNC_TIMEOUT_MS = 100;

static HardwareSerial *s_uart = nullptr;

// Sticky DFU session state — see openSession()/closeSession() below.
static bool     s_session_open     = false;
static uint32_t s_session_last_use = 0;
// True when the current sticky session has uploaded the esptool stub
// flasher into RX RAM and ROM has jumped to it. Stub responds to the same
// SLIP protocol as ROM but without the autobauder fragility — mixed
// READ_REG / READ_FLASH_SLOW chains work reliably. See loadStub() below.
static bool     s_session_stub_loaded = false;

// ---- SLIP send ----
static void slipStart() { s_uart->write(0xC0); }
static void slipEnd() { s_uart->write(0xC0); }
static void slipWriteByte(uint8_t b) {
    if (b == 0xC0) { s_uart->write(0xDB); s_uart->write(0xDC); }
    else if (b == 0xDB) { s_uart->write(0xDB); s_uart->write(0xDD); }
    else s_uart->write(b);
}
static void slipWrite(const uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) slipWriteByte(buf[i]);
}

// ---- SLIP read ----
// Read one SLIP frame into buf, returns length or -1 on timeout
static int slipRead(uint8_t *buf, size_t maxLen, uint32_t timeoutMs) {
    uint32_t start = millis();
    bool in_frame = false;
    size_t len = 0;

    while ((millis() - start) < timeoutMs) {
        if (!s_uart->available()) { delay(1); continue; }
        uint8_t b = s_uart->read();

        if (b == 0xC0) {
            if (!in_frame) { in_frame = true; continue; }
            if (len == 0) continue; // empty frame
            return (int)len;
        }
        if (!in_frame) continue;

        if (b == 0xDB) {
            // Wait for next byte
            while (!s_uart->available() && (millis() - start) < timeoutMs) delay(1);
            if (!s_uart->available()) return -1;
            uint8_t e = s_uart->read();
            if (e == 0xDC) b = 0xC0;
            else if (e == 0xDD) b = 0xDB;
            else b = e;
        }
        if (len < maxLen) buf[len++] = b;
    }
    return -1;
}

// Compute XOR checksum (initial seed = 0xEF)
static uint8_t cksum(const uint8_t *data, size_t n) {
    uint8_t c = 0xEF;
    for (size_t i = 0; i < n; i++) c ^= data[i];
    return c;
}

// Send command + wait for response. Returns true on OK status.
// response_value returns the 4-byte "value" field
static bool sendCmd(uint8_t cmd, const uint8_t *data, uint16_t size,
                   uint32_t checksum, uint32_t timeoutMs,
                   uint32_t *response_value = nullptr) {
    // Flush any pending bytes from a previous frame — avoids reading
    // a stale tail as if it were the current response.
    while (s_uart->available()) s_uart->read();

    // Build packet: direction, cmd, size_lo, size_hi, cksum x4, data
    slipStart();
    slipWriteByte(0x00);
    slipWriteByte(cmd);
    slipWriteByte(size & 0xFF);
    slipWriteByte((size >> 8) & 0xFF);
    slipWriteByte(checksum & 0xFF);
    slipWriteByte((checksum >> 8) & 0xFF);
    slipWriteByte((checksum >> 16) & 0xFF);
    slipWriteByte((checksum >> 24) & 0xFF);
    slipWrite(data, size);
    slipEnd();
    s_uart->flush();

    // Read response
    static uint8_t buf[512];
    int n = slipRead(buf, sizeof(buf), timeoutMs);
    if (n < 8) return false;
    if (buf[0] != 0x01 || buf[1] != cmd) return false;

    uint16_t resp_size = buf[2] | (buf[3] << 8);
    uint32_t value = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                     ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    if (response_value) *response_value = value;

    // Last 2 bytes = status. Truncated frame → failure. SYNC is the only
    // command with no status bytes, and it drains its replies via a separate
    // loop in sync(), so reaching here with a short frame means a real
    // communication drop (usually the 256 B default UART RX ring overflowed).
    if (resp_size < 2 || n < 8 + resp_size) return false;
    uint8_t status = buf[8 + resp_size - 2];
    uint8_t err = buf[8 + resp_size - 1];
    (void)err;
    return status == 0x00;
}

// Sync with ROM bootloader
static bool sync() {
    // SYNC packet: 0x07 0x07 0x12 0x20 + 32 bytes of 0x55
    uint8_t syncData[36];
    syncData[0] = 0x07; syncData[1] = 0x07;
    syncData[2] = 0x12; syncData[3] = 0x20;
    for (int i = 4; i < 36; i++) syncData[i] = 0x55;

    uint32_t ck = cksum(syncData, 36);

    // Flush any pending bytes
    while (s_uart->available()) s_uart->read();

    // Try sync up to 10 times
    for (int attempt = 0; attempt < 10; attempt++) {
        if (sendCmd(CMD_SYNC, syncData, 36, ck, SYNC_TIMEOUT_MS)) {
            // Read any additional sync replies (ROM sends 8 total)
            uint8_t discard[64];
            for (int i = 0; i < 7; i++) {
                slipRead(discard, sizeof(discard), 100);
            }
            return true;
        }
        delay(50);
    }
    return false;
}

// Erase region and prepare for writing.
// ESP32-S2/S3/C3/C6 ROMs expect a 20-byte payload (extra 4 bytes =
// encryption_flag). Older ESP8266 / original ESP32 ROM used 16 bytes.
// Sending 16 to a C3 ROM silently misparses the offset — flash "succeeds"
// but nothing lands. We always send 20 bytes with encryption=0 — works on
// all modern ROMs and is ignored by older ones.
static bool flashBegin(uint32_t size, uint32_t offset) {
    uint32_t block_size = FLASH_WRITE_BLOCK_SIZE;
    uint32_t num_blocks = (size + block_size - 1) / block_size;
    uint32_t erase_size = num_blocks * block_size;

    uint8_t data[20];
    *(uint32_t*)(data + 0)  = erase_size;
    *(uint32_t*)(data + 4)  = num_blocks;
    *(uint32_t*)(data + 8)  = block_size;
    *(uint32_t*)(data + 12) = offset;
    *(uint32_t*)(data + 16) = 0;   // encrypted = 0 (plaintext flash)

    uint32_t ck = cksum(data, 20);
    // ESP32-C3 ROM erases ~40 KB/s — for 1 MB+ writes the pre-erase can
    // take 30+ seconds. Give it 90s (covers up to ~3.5 MB). Without this,
    // sendCmd() times out at 10s, we assume FLASH_BEGIN succeeded, then
    // subsequent FLASH_DATA writes land in NOT-yet-erased sectors and are
    // silently dropped. Observed symptom: first ~288 KB write OK, rest
    // stays 0xFF.
    return sendCmd(CMD_FLASH_BEGIN, data, 20, ck, 90000);
}

// Write one block of flash data (block_size must match flashBegin)
static bool flashData(const uint8_t *block_data, size_t block_size, uint32_t seq) {
    // Header (16 bytes) + data
    static uint8_t packet[16 + FLASH_WRITE_BLOCK_SIZE];

    *(uint32_t*)(packet + 0) = block_size;
    *(uint32_t*)(packet + 4) = seq;
    *(uint32_t*)(packet + 8) = 0;
    *(uint32_t*)(packet + 12) = 0;
    memcpy(packet + 16, block_data, block_size);

    // Pad with 0xFF if block is smaller than FLASH_WRITE_BLOCK_SIZE
    if (block_size < FLASH_WRITE_BLOCK_SIZE) {
        memset(packet + 16 + block_size, 0xFF, FLASH_WRITE_BLOCK_SIZE - block_size);
    }

    // Checksum is over the DATA only (per esptool.py)
    uint32_t ck = cksum(packet + 16, FLASH_WRITE_BLOCK_SIZE);

    // Send full packet (header + padded data)
    return sendCmd(CMD_FLASH_DATA, packet, 16 + FLASH_WRITE_BLOCK_SIZE, ck, 3000);
}

// Finish flashing and optionally reboot
static bool flashEnd(bool reboot) {
    uint8_t data[4];
    // 0 = reboot, 1 = stay in bootloader
    *(uint32_t*)data = reboot ? 0 : 1;
    uint32_t ck = cksum(data, 4);
    return sendCmd(CMD_FLASH_END, data, 4, ck, 3000);
}

// Forward declarations for helpers defined below.
static bool spiAttach();
static bool spiSetParams(uint32_t flash_size_bytes);

// Sticky session: open Serial1 + sync + spiAttach + spiSetParams ONCE so
// callers can chain readFlashMultiInOpenSession() / chipInfoInOpenSession() /
// spiFlashMd5InOpenSession() / eraseRegionMultiInOpenSession() without ever
// closing Serial1 between operations. This sidesteps the ESP32-C3 ROM
// autobauder quirk (BUG-ID1) where Serial1.end()+begin() between two
// SLIP exchanges fails the second sync.
//
// Idempotent if already open: returns FLASH_OK without re-syncing. Caller
// must closeSession() first to reconfigure baud / pins.
Result openSession(const Config &cfg, uint32_t flash_size_bytes) {
    if (s_session_open) return FLASH_OK;
    if (!cfg.uart) return FLASH_ERR_INVALID_INPUT;
    s_uart = cfg.uart;
    s_uart->setRxBufferSize(8192);
    s_uart->begin(cfg.baud_rate, SERIAL_8N1, cfg.rx_pin, cfg.tx_pin);
    delay(100);
    if (!sync()) { s_uart->end(); return FLASH_ERR_NO_SYNC; }
    spiAttach();
    if (flash_size_bytes < 0x400000) flash_size_bytes = 0x400000;
    spiSetParams(flash_size_bytes);
    s_session_open = true;
    s_session_last_use = millis();
    return FLASH_OK;
}

void closeSession() {
    if (!s_session_open) return;
    if (s_uart) s_uart->end();
    s_session_open = false;
    // Stub lives in RX RAM only while RX is powered; nothing to "unload".
    // Clearing the flag prevents a future openSession() from assuming stub
    // is still loaded — fresh session must re-upload if it wants stub mode.
    s_session_stub_loaded = false;
}

bool sessionOpen() { return s_session_open; }
void touchSession() { if (s_session_open) s_session_last_use = millis(); }
bool sessionIdleSince(uint32_t ms) {
    return s_session_open && (millis() - s_session_last_use >= ms);
}
bool sessionStubLoaded() { return s_session_stub_loaded; }

// MEM_BEGIN: tell ROM to allocate `size` bytes in RAM at `offset`, expecting
// `num_blocks` writes of `block_size` each. Returns ROM ack.
static bool memBegin(uint32_t size, uint32_t num_blocks,
                     uint32_t block_size, uint32_t offset) {
    uint8_t data[16];
    *(uint32_t*)(data + 0)  = size;
    *(uint32_t*)(data + 4)  = num_blocks;
    *(uint32_t*)(data + 8)  = block_size;
    *(uint32_t*)(data + 12) = offset;
    return sendCmd(CMD_MEM_BEGIN, data, 16, cksum(data, 16), 3000);
}

// MEM_DATA: write one block of stub binary to the allocated RAM region.
// Header (16 B) + payload. Payload must be exactly block_size as declared
// in memBegin (ROM doesn't tolerate short writes here, unlike FLASH_DATA).
static bool memData(const uint8_t *block, size_t block_size, uint32_t seq) {
    static uint8_t pkt[16 + 1536];
    if (block_size > 1536) return false;
    *(uint32_t*)(pkt + 0)  = block_size;
    *(uint32_t*)(pkt + 4)  = seq;
    *(uint32_t*)(pkt + 8)  = 0;
    *(uint32_t*)(pkt + 12) = 0;
    memcpy(pkt + 16, block, block_size);
    uint32_t ck = cksum(pkt + 16, block_size);
    return sendCmd(CMD_MEM_DATA, pkt, 16 + block_size, ck, 3000);
}

// MEM_END: commit the upload and either (a) jump to entry_point if stay=0,
// or (b) leave it loaded and return control if stay=1. We always use stay=0
// because the only point of uploading is to run the stub.
//
// IMPORTANT: ROM's reply to MEM_END comes BEFORE the jump. After we read it,
// ROM transfers control to entry_point — at which point the stub starts up
// and emits a 6-byte SLIP frame containing "OHAI" (\x4f\x48\x41\x49). We
// consume that as the handshake confirming the stub is alive.
static bool memEnd(uint32_t entry_point) {
    uint8_t data[8];
    *(uint32_t*)(data + 0) = 0;            // stay_in_loader = 0 → jump
    *(uint32_t*)(data + 4) = entry_point;
    return sendCmd(CMD_MEM_END, data, 8, cksum(data, 8), 3000);
}

// Wait for the stub's startup handshake — a SLIP-framed "OHAI" payload
// (4 bytes, 0x4f 0x48 0x41 0x49). esptool calls this the "OHAI" probe.
// Returns true if seen within timeout_ms.
static bool waitStubOhai(uint32_t timeout_ms) {
    uint8_t buf[16];
    uint32_t deadline = millis() + timeout_ms;
    while (millis() < deadline) {
        int n = slipRead(buf, sizeof(buf), 100);
        if (n >= 4 && buf[0] == 0x4f && buf[1] == 0x48
                  && buf[2] == 0x41 && buf[3] == 0x49) {
            return true;
        }
    }
    return false;
}

// Upload the embedded stub matching `chip_magic` and jump to its entry
// point. After this, ROM is dormant and stub answers all subsequent SLIP
// commands. Stub IS robust to mixed READ_REG/READ_FLASH/MD5 sequences —
// that's the whole reason this exists.
//
// Sequence (per esptool LoadELRSESP32C3.run_stub):
//   1. memBegin(text_len, ceil(text_len/block_size), block_size, text_start)
//   2. memData(seq, block_size, text_chunk) × num_blocks
//   3. memBegin(data_len, ceil(data_len/block_size), block_size, data_start)
//   4. memData(seq, block_size, data_chunk) × num_blocks
//   5. memEnd(entry_point) → ROM jumps, stub starts
//   6. waitStubOhai() → confirms stub alive
//
// 1024-byte blocks for both segments. ESP32-S3 stub is 4836 B text → 5 blocks.
Result loadStub(uint32_t chip_magic) {
    if (!s_session_open) return FLASH_ERR_INVALID_INPUT;
    if (s_session_stub_loaded) return FLASH_OK;  // idempotent
    const ESPRomStub::StubBin *stub = ESPRomStub::findStubByMagic(chip_magic);
    if (!stub) return FLASH_ERR_INVALID_INPUT;
    s_session_last_use = millis();

    static const uint32_t BLOCK = 1024;

    auto upload_segment = [&](const uint8_t *src_pgm, uint32_t len, uint32_t addr) -> bool {
        uint32_t num_blocks = (len + BLOCK - 1) / BLOCK;
        if (!memBegin(len, num_blocks, BLOCK, addr)) return false;
        // Stub binaries live in PROGMEM — copy to a tmp buffer block-by-block.
        static uint8_t blk[BLOCK];
        for (uint32_t i = 0; i < num_blocks; i++) {
            uint32_t off = i * BLOCK;
            uint32_t sz  = (len - off > BLOCK) ? BLOCK : (len - off);
            memcpy_P(blk, src_pgm + off, sz);
            // ROM expects exactly block_size bytes regardless of actual
            // payload — pad with 0 if the last block is short.
            if (sz < BLOCK) memset(blk + sz, 0x00, BLOCK - sz);
            if (!memData(blk, BLOCK, i)) return false;
            s_session_last_use = millis();
        }
        return true;
    };

    if (!upload_segment(stub->text, stub->text_len, stub->text_start))
        return FLASH_ERR_WRITE_FAILED;
    if (stub->data_len > 0 &&
        !upload_segment(stub->data, stub->data_len, stub->data_start))
        return FLASH_ERR_WRITE_FAILED;

    if (!memEnd(stub->entry)) return FLASH_ERR_END_FAILED;
    // ROM has jumped to the stub. The stub's first action is to send "OHAI".
    if (!waitStubOhai(2000)) return FLASH_ERR_NO_SYNC;

    // Stub starts with its own SPI peripheral state — ROM's prior
    // spiAttach() + spiSetParams() do NOT carry over. Re-initialize so
    // subsequent READ_FLASH_SLOW / FLASH_BEGIN / SPI_FLASH_MD5 commands
    // can talk to flash. Without this, reads silently fail (data area
    // empty) while register-only commands (READ_REG, MD5 already-cached)
    // appear to work — leading to confusing partial breakage.
    spiAttach();
    spiSetParams(0x400000);

    s_session_stub_loaded = true;
    s_session_last_use = millis();
    return FLASH_OK;
}

// In-session variants — UART/sync/spi setup is assumed done. Each call
// touches s_session_last_use on success so the idle watchdog resets.

// Stub-only fast read: CMD_READ_FLASH (0xD2) streaming protocol. ESP32-C3
// stub does NOT implement CMD_READ_FLASH_SLOW (0x0E) — only the streaming
// 0xD2. Protocol per esptool 4.x:
//   1. Send CMD_READ_FLASH with payload [offset:4, size:4, pkt_sz:4, max_inflight:4]
//   2. Read normal command-status response.
//   3. Repeat: read SLIP frame (data packet) → send 4-byte ack (total bytes
//      received so far) wrapped in SLIP. Loop until `size` bytes collected.
//   4. Read SLIP frame containing 16-byte MD5 trailer.
//   5. Send final 4-byte ack.
// Returns FLASH_OK / FLASH_ERR_READ_FAILED. ~5–10× faster than SLOW.
static Result readFlashStream(uint32_t offset, uint32_t size, uint8_t *out) {
    // Match esptool 4.x defaults exactly — stub validates these args and
    // rejects the command if they don't fit its expectations:
    //   FLASH_SECTOR_SIZE = 0x1000 (4 KB)  → packet size
    //   max_inflight      = 64             → window size
    static const uint32_t PKT_SZ       = 0x1000;
    static const uint32_t MAX_INFLIGHT = 64;

    uint8_t req[16];
    *(uint32_t*)(req + 0)  = offset;
    *(uint32_t*)(req + 4)  = size;
    *(uint32_t*)(req + 8)  = PKT_SZ;
    *(uint32_t*)(req + 12) = MAX_INFLIGHT;
    // DEBUG: capture the raw response into the global last-debug buffer
    // (read via /api/elrs/dfu/debug) so we can see WHY it's rejected.
    while (s_uart->available()) s_uart->read();
    slipStart();
    slipWriteByte(0x00);
    slipWriteByte(CMD_READ_FLASH);
    slipWriteByte(16); slipWriteByte(0);
    // chk=0 for non-data commands (esptool only validates chk on
    // FLASH_DATA / MEM_DATA). Earlier we were sending cksum(req,16) which
    // matched our own pattern but might be what stub treats as "wrong".
    slipWriteByte(0); slipWriteByte(0); slipWriteByte(0); slipWriteByte(0);
    slipWrite(req, 16);
    slipEnd();
    s_uart->flush();
    static uint8_t dbg[64];
    int dn = slipRead(dbg, sizeof(dbg), 5000);
    if (::g_dfu_debug_buf) {
        size_t off = snprintf(::g_dfu_debug_buf, 256,
            "sent: cmd=%02x size=16 chk=0 args=ofs=%08x sz=%08x pkt=%08x inf=%08x\n"
            "stub_resp len=%d",
            CMD_READ_FLASH, (unsigned)offset, (unsigned)size, (unsigned)PKT_SZ,
            (unsigned)MAX_INFLIGHT, dn);
        if (dn > 0 && off < 200) {
            off += snprintf(::g_dfu_debug_buf + off, 256 - off, " hex=");
            for (int i = 0; i < dn && i < 24 && off < 250; i++) {
                off += snprintf(::g_dfu_debug_buf + off, 256 - off, "%02x ", dbg[i]);
            }
        }
        ::g_dfu_debug_len = off;
    }
    if (dn < 8 || dbg[0] != 0x01 || dbg[1] != CMD_READ_FLASH) {
        return FLASH_ERR_READ_FAILED;
    }
    uint16_t rsz = dbg[2] | (dbg[3] << 8);
    if (rsz >= 2 && dn >= 8 + rsz) {
        uint8_t st = dbg[8 + rsz - 2];
        if (st != 0) return FLASH_ERR_READ_FAILED;
    }

    auto sendAck = [](uint32_t total) {
        uint8_t a[4];
        *(uint32_t*)a = total;
        slipStart();
        for (int i = 0; i < 4; i++) slipWriteByte(a[i]);
        slipEnd();
        s_uart->flush();
    };

    // PSRAM buf for one packet — too big for stack/static at 4K+ on small
    // RAM. esp_heap_caps_malloc not available here → use a static; 4160 B
    // is fine in the .bss for an S3 with 8 MB PSRAM but tight on a small
    // chip. Keeping it static keeps the function reentrancy-free, which is
    // OK because the session is single-threaded by PinPort.
    static uint8_t pkt[PKT_SZ + 64];
    uint32_t got = 0;
    while (got < size) {
        int n = slipRead(pkt, sizeof(pkt), 5000);
        if (n <= 0) return FLASH_ERR_READ_FAILED;
        uint32_t want = size - got;
        uint32_t copy = ((uint32_t)n > want) ? want : (uint32_t)n;
        memcpy(out + got, pkt, copy);
        got += copy;
        sendAck(got);
    }
    // MD5 trailer (16 bytes raw or 32 hex chars — consume but don't ack).
    // esptool's read_flash does NOT send a finish-ack after MD5; an extra
    // ack confuses stub's command loop and breaks the NEXT readFlashStream
    // call (observed: 1st call OK, 2nd returns cmd=0x10 status=0x01).
    static uint8_t md5[40];
    int mn = slipRead(md5, sizeof(md5), 5000);
    if (mn <= 0) return FLASH_ERR_READ_FAILED;
    return FLASH_OK;
}

Result readFlashMultiInOpenSession(const ReadRegion *regions, size_t n) {
    if (!s_session_open || !regions || n == 0) return FLASH_ERR_INVALID_INPUT;
    s_session_last_use = millis();

    // Stub mode: use streaming CMD_READ_FLASH (0xD2). Fresh stub binaries
    // from PIO's tool-esptoolpy correctly implement this opcode (older
    // ELRS-vendored copies did not — the data segment was 8 B vs 216 B).
    // ROM (no stub) keeps the slow CMD_READ_FLASH_SLOW (0x0E) path below.
    if (s_session_stub_loaded) {
        for (size_t ri = 0; ri < n; ri++) {
            const ReadRegion &r = regions[ri];
            if (!r.dst || r.size == 0) return FLASH_ERR_INVALID_INPUT;
            Result rs = readFlashStream(r.offset, r.size, r.dst);
            if (rs != FLASH_OK) return rs;
            s_session_last_use = millis();
        }
        return FLASH_OK;
    }

    static const uint32_t CHUNK = 64;
    static uint8_t resp[128];
    for (size_t ri = 0; ri < n; ri++) {
        const ReadRegion &r = regions[ri];
        if (!r.dst || r.size == 0) return FLASH_ERR_INVALID_INPUT;
        size_t received = 0;
        while (received < r.size) {
            uint32_t take = (r.size - received > CHUNK) ? CHUNK : (r.size - received);
            uint8_t req[8];
            *(uint32_t*)(req + 0) = r.offset + (uint32_t)received;
            *(uint32_t*)(req + 4) = take;
            uint32_t ck = cksum(req, 8);
            slipStart();
            slipWriteByte(0x00);
            slipWriteByte(CMD_READ_FLASH_SLOW);
            slipWriteByte(8 & 0xFF);
            slipWriteByte((8 >> 8) & 0xFF);
            slipWriteByte(ck & 0xFF);
            slipWriteByte((ck >> 8) & 0xFF);
            slipWriteByte((ck >> 16) & 0xFF);
            slipWriteByte((ck >> 24) & 0xFF);
            slipWrite(req, 8);
            slipEnd();
            s_uart->flush();
            int nr = slipRead(resp, sizeof(resp), 2000);
            if (nr < 8 || resp[0] != 0x01 || resp[1] != CMD_READ_FLASH_SLOW)
                return FLASH_ERR_READ_FAILED;
            uint16_t resp_size = resp[2] | (resp[3] << 8);
            int data_len = (int)resp_size - 2;
            if (data_len <= 0 || 8 + data_len > nr) return FLASH_ERR_READ_FAILED;
            if ((uint32_t)data_len > take) data_len = (int)take;
            memcpy(r.dst + received, resp + 8, data_len);
            received += data_len;
            // Refresh idle marker every chunk — long reads (192 KB SPIFFS
            // ≈ 16 s wall-clock) would otherwise let the 60 s idle watchdog
            // fire mid-call.
            s_session_last_use = millis();
        }
    }
    return FLASH_OK;
}

Result eraseRegionMultiInOpenSession(const EraseRegion *regions, size_t n) {
    if (!s_session_open || !regions || n == 0) return FLASH_ERR_INVALID_INPUT;
    s_session_last_use = millis();
    for (size_t i = 0; i < n; i++) {
        if (regions[i].size == 0) continue;
        if (!flashBegin(regions[i].size, regions[i].offset))
            return FLASH_ERR_BEGIN_FAILED;
        (void)flashEnd(false);
        s_session_last_use = millis();
    }
    return FLASH_OK;
}

Result spiFlashMd5InOpenSession(uint32_t offset, uint32_t size, uint8_t out[16]) {
    if (!s_session_open || !out || size == 0) return FLASH_ERR_INVALID_INPUT;
    s_session_last_use = millis();
    // Re-sync + re-init SPI peripheral. After a long FLASH_DATA chain, ROM
    // leaves SPI in "flash-data mode" — SPI_FLASH_MD5 silently fails
    // ("READ_FLASH rejected") until SPI_ATTACH + SPI_SET_PARAMS re-arm it.
    // sync() also recovers from any autobauder hiccup the chain produced.
    if (!sync()) return FLASH_ERR_NO_SYNC;
    spiAttach();
    uint32_t fs = offset + size;
    if (fs < 0x400000) fs = 0x400000;
    spiSetParams(fs);

    uint8_t data[16];
    *(uint32_t*)(data + 0)  = offset;
    *(uint32_t*)(data + 4)  = size;
    *(uint32_t*)(data + 8)  = 0;
    *(uint32_t*)(data + 12) = 0;
    uint32_t ck = cksum(data, 16);
    slipStart();
    slipWriteByte(0x00);
    slipWriteByte(CMD_SPI_FLASH_MD5);
    slipWriteByte(16); slipWriteByte(0);
    slipWriteByte(ck & 0xFF);
    slipWriteByte((ck >> 8) & 0xFF);
    slipWriteByte((ck >> 16) & 0xFF);
    slipWriteByte((ck >> 24) & 0xFF);
    slipWrite(data, 16);
    slipEnd();
    s_uart->flush();

    static uint8_t buf[128];
    int n = slipRead(buf, sizeof(buf), 10000);
    if (n < 8 || buf[0] != 0x01 || buf[1] != CMD_SPI_FLASH_MD5)
        return FLASH_ERR_READ_FAILED;
    uint16_t resp_size = buf[2] | (buf[3] << 8);
    if (n < 8 + resp_size) return FLASH_ERR_READ_FAILED;
    uint8_t status = buf[8 + resp_size - 2];
    if (status != 0x00) return FLASH_ERR_READ_FAILED;
    uint8_t *md5_area = &buf[8];
    uint16_t md5_len = resp_size - 2;
    if (md5_len == 16) {
        memcpy(out, md5_area, 16);
    } else if (md5_len == 32) {
        auto h2i = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        for (int i = 0; i < 16; i++)
            out[i] = (uint8_t)((h2i(md5_area[i*2]) << 4) | h2i(md5_area[i*2 + 1]));
    } else {
        return FLASH_ERR_READ_FAILED;
    }
    s_session_last_use = millis();
    return FLASH_OK;
}

void flashEndInOpenSession(bool reboot) {
    if (!s_session_open) return;
    (void)flashEnd(reboot);
    s_session_last_use = millis();
}

Result chipInfoInOpenSession(ChipInfo *out) {
    if (!s_session_open || !out) return FLASH_ERR_INVALID_INPUT;
    s_session_last_use = millis();
    memset(out, 0, sizeof(*out));
    out->chip_name = "unknown";

    // Re-sync before READ_REG. After a long READ_FLASH_SLOW chain
    // (identity/fast = 5 regions × ~64 chunks each), ESP32-C3 ROM observed
    // to reject CMD_READ_REG with no reply. SYNC inside the SAME Serial1
    // session is cheap (~50 ms) and reproducibly restores ROM responsiveness
    // — the autobauder-latch issue (BUG-ID1) only happens across Serial1
    // end/begin, not within an open session.
    if (!sync()) return FLASH_ERR_NO_SYNC;

    auto readReg = [](uint32_t addr, uint32_t *val) -> bool {
        uint8_t req[4];
        *(uint32_t*)req = addr;
        return sendCmd(CMD_READ_REG, req, 4, cksum(req, 4), 3000, val);
    };

    uint32_t magic = 0;
    if (!readReg(0x40001000, &magic)) return FLASH_ERR_READ_FAILED;
    out->magic_value = magic;
    switch (magic) {
        case 0xfff0c101: out->chip_name = "ESP8266"; break;
        case 0x00f01d83: out->chip_name = "ESP32"; break;
        case 0x000007c6: out->chip_name = "ESP32-S2"; break;
        case 0x00000009:
        case 0xeb004136: out->chip_name = "ESP32-S3"; break;
        case 0x6921506f:
        case 0x1b31506f: out->chip_name = "ESP32-C3"; break;
        case 0x0da1806f: out->chip_name = "ESP32-C6"; break;
        case 0xd7b73e80: out->chip_name = "ESP32-H2"; break;
        default:         out->chip_name = "unknown"; break;
    }
    out->ok = true;

    if (magic == 0x6921506f || magic == 0x1b31506f) {
        uint32_t m0 = 0, m1 = 0;
        if (readReg(0x60008844, &m0) && readReg(0x60008848, &m1)) {
            out->mac[0] = (uint8_t)(m1 >> 8);
            out->mac[1] = (uint8_t)(m1 >> 0);
            out->mac[2] = (uint8_t)(m0 >> 24);
            out->mac[3] = (uint8_t)(m0 >> 16);
            out->mac[4] = (uint8_t)(m0 >> 8);
            out->mac[5] = (uint8_t)(m0 >> 0);
        }
    }
    s_session_last_use = millis();
    return FLASH_OK;
}

// Main flash entry point.
// Single FLASH_BEGIN for the entire image, then N × FLASH_DATA blocks.
// Earlier versions chunked at 256 KB because with 1 KB blocks the ROM hit a
// block-count cliff at 287 blocks (~287 KB). With 4 KB blocks that same
// block-count limit maps to ~1.1 MB — past any single ELRS image we flash.
// Chunking itself turned out to break on the second chunk's FLASH_BEGIN
// (timed out after 90 s), so single-session is both simpler and correct.
// Read up to 256 bytes from flash inside an already-open session (Serial1
// is live, RX is in DFU). Avoids losing DFU between write and verify — the
// separate readFlash() path does its own begin/sync/end, and empirically
// that end() triggers the RX to exit DFU under some ROM timing.
// Caller must have already done sync + spiAttach + spiSetParams earlier in
// the session. Uses CMD_READ_FLASH_SLOW (0x0E) with 64 B chunks.
static bool readFlashInSession(uint32_t offset, uint32_t size, uint8_t *out) {
    static const uint32_t CHUNK = 64;
    static uint8_t resp[128];
    uint32_t received = 0;
    while (received < size) {
        uint32_t take = (size - received > CHUNK) ? CHUNK : (size - received);
        uint8_t req[8];
        *(uint32_t*)(req + 0) = offset + received;
        *(uint32_t*)(req + 4) = take;
        uint32_t ck = cksum(req, 8);

        slipStart();
        slipWriteByte(0x00);
        slipWriteByte(CMD_READ_FLASH_SLOW);
        slipWriteByte(8); slipWriteByte(0);
        slipWriteByte(ck & 0xFF);
        slipWriteByte((ck >> 8) & 0xFF);
        slipWriteByte((ck >> 16) & 0xFF);
        slipWriteByte((ck >> 24) & 0xFF);
        slipWrite(req, 8);
        slipEnd();
        s_uart->flush();

        int n = slipRead(resp, sizeof(resp), 2000);
        if (n < 8 || resp[0] != 0x01 || resp[1] != CMD_READ_FLASH_SLOW) return false;
        uint16_t resp_size = resp[2] | (resp[3] << 8);
        int data_len = (int)resp_size - 2;
        if (data_len <= 0 || 8 + data_len > n) return false;
        if ((uint32_t)data_len > take) data_len = (int)take;
        memcpy(out + received, resp + 8, data_len);
        received += data_len;
    }
    return true;
}

Result flash(const Config &cfg, const uint8_t *data, size_t size,
             Sample *samples, size_t n_samples) {
    if (!cfg.uart || !data || size == 0) return FLASH_ERR_INVALID_INPUT;

    // Honour an externally-opened sticky session — flash() then becomes
    // re-entrant within /dfu/begin/end and the caller can chain a post-
    // flash spiFlashMd5InOpenSession() in the SAME Serial1 cycle (BUG-ID1
    // fix on the write path: previously the second sync after Serial1.end
    // failed with "No sync" and full-image MD5 verify silently degraded).
    bool external_session = sessionOpen();

    if (!external_session) {
        s_uart = cfg.uart;
        // setRxBufferSize MUST be called before begin() — Arduino-ESP32
        // allocates the UART ring during begin() and ignores later resizes
        // until the next end()/begin() cycle. A stuck-at-256 ring overflows
        // mid-frame at 115200 baud and desyncs the SLIP parser.
        s_uart->setRxBufferSize(4096);
        s_uart->begin(cfg.baud_rate, SERIAL_8N1, cfg.rx_pin, cfg.tx_pin);
        delay(100);

        if (cfg.progress) cfg.progress(0, "Connecting");

        if (!sync()) {
            s_uart->end();
            return FLASH_ERR_NO_SYNC;
        }
        if (cfg.progress) cfg.progress(5, "Synced");

        spiAttach();
        uint32_t fs = cfg.flash_offset + (uint32_t)size;
        if (fs < 0x400000) fs = 0x400000;
        spiSetParams(fs);
    } else {
        if (cfg.progress) cfg.progress(5, "Reusing session");
        // sessionOpen()-true implies sync + spiAttach + spiSetParams already
        // ran. We assume cfg matches what openSession() was called with.
        s_session_last_use = millis();
    }

    auto _maybe_end = [&]() { if (!external_session) s_uart->end(); };

    if (!flashBegin(size, cfg.flash_offset)) {
        _maybe_end();
        return FLASH_ERR_BEGIN_FAILED;
    }
    if (cfg.progress) cfg.progress(10, "Erasing");

    uint32_t num_blocks = (size + FLASH_WRITE_BLOCK_SIZE - 1) / FLASH_WRITE_BLOCK_SIZE;
    for (uint32_t i = 0; i < num_blocks; i++) {
        size_t chunk_off = (size_t)i * FLASH_WRITE_BLOCK_SIZE;
        size_t block_size = (size - chunk_off) < FLASH_WRITE_BLOCK_SIZE
            ? (size - chunk_off)
            : FLASH_WRITE_BLOCK_SIZE;
        if (!flashData(data + chunk_off, block_size, i)) {
            _maybe_end();
            return FLASH_ERR_WRITE_FAILED;
        }
        // Throttle between blocks. ESP32-C3 ROM's FLASH_DATA handler acks on
        // receive, not on commit — NOR flash writes at ~40 KB/s, so each 4 KB
        // block needs ~100 ms to physically land. Without throttle, the ROM's
        // internal write queue overflows somewhere beyond ~256 KB and
        // subsequent ACKs become lies. Wire time alone is ~365 ms/block at
        // 115200 baud, so ~30 s of added delay is cheap insurance.
        delay(100);
        if (cfg.progress) {
            int pct = 10 + (int)((chunk_off + block_size) * 85 / size);
            cfg.progress(pct, "Writing");
        }
        if (external_session) s_session_last_use = millis();
    }

    // In-session sample readback BEFORE FLASH_END/Serial1.end(). The RX
    // tends to exit DFU if the UART goes silent between our commands
    // (suspected ROM watchdog / auto-baud retrigger), so we verify while
    // we still have the session open.
    if (samples && n_samples > 0) {
        if (cfg.progress) cfg.progress(97, "Verifying");
        for (size_t i = 0; i < n_samples; i++) {
            if (samples[i].size == 0 || samples[i].size > sizeof(samples[i].data)) {
                samples[i].ok = false;
                continue;
            }
            samples[i].ok = readFlashInSession(samples[i].offset, samples[i].size, samples[i].data);
        }
    }

    // FLASH_END policy:
    //  - Standalone + !stay_in_loader → FLASH_END(true) (reboot, close UART).
    //  - External session → SKIP FLASH_END entirely. ESP32-C3 ROM observed
    //    to auto-reset on ANY FLASH_END regardless of payload (out-of-DFU);
    //    sticking around in DFU requires the caller to send SPI_ATTACH +
    //    SPI_SET_PARAMS itself before SPI_FLASH_MD5 to re-init the SPI
    //    peripheral after the FLASH_DATA chain.
    if (!external_session && !cfg.stay_in_loader) {
        if (!flashEnd(true)) {
            // Non-fatal: some bootloaders don't respond to FLASH_END.
        }
    }

    if (cfg.progress) cfg.progress(100, "Done");
    _maybe_end();
    return FLASH_OK;
}

const char* errorString(Result r) {
    switch (r) {
        case FLASH_OK:               return "OK";
        case FLASH_ERR_NO_SYNC:      return "No sync (is device in bootloader mode?)";
        case FLASH_ERR_BEGIN_FAILED: return "FLASH_BEGIN failed";
        case FLASH_ERR_WRITE_FAILED: return "FLASH_DATA failed";
        case FLASH_ERR_END_FAILED:   return "FLASH_END failed";
        case FLASH_ERR_TIMEOUT:      return "Timeout";
        case FLASH_ERR_INVALID_INPUT: return "Invalid input";
        case FLASH_ERR_READ_FAILED:  return "READ_FLASH rejected or bad response";
        default: return "Unknown";
    }
}

// SPI_ATTACH prepares the ROM SPI flash driver so subsequent READ_FLASH works.
// On ESP32-S2/S3/C3 the argument is [spi_config:4, zero:4] (default cfg=0).
static bool spiAttach() {
    uint8_t data[8] = {0};
    return sendCmd(CMD_SPI_ATTACH, data, 8, cksum(data, 8), 3000);
}

// SPI_SET_PARAMS tells the ROM the flash geometry. ESP32-C3 ROM READ_FLASH
// refuses without this (we saw "READ_FLASH rejected" until we added it).
// Args: [id:4, total_size:4, block_size:4, sector_size:4, page_size:4, status_mask:4]
// We default to 4 MB / 64 KB blocks / 4 KB sectors / 256 B pages — standard
// for most 2-16 MB SPI flash parts, including Bayck RC C3 Dual.
static bool spiSetParams(uint32_t flash_size_bytes) {
    uint8_t data[24];
    memset(data, 0, sizeof(data));
    *(uint32_t*)(data + 0)  = 0;               // device id (unused)
    *(uint32_t*)(data + 4)  = flash_size_bytes;
    *(uint32_t*)(data + 8)  = 0x00010000;      // block size (64 KB)
    *(uint32_t*)(data + 12) = 0x00001000;      // sector size (4 KB)
    *(uint32_t*)(data + 16) = 0x00000100;      // page size (256 B)
    *(uint32_t*)(data + 20) = 0x0000FFFF;      // status mask
    return sendCmd(CMD_SPI_SET_PARAMS, data, 24, cksum(data, 24), 3000);
}

// Erase a flash region without writing anything. Uses FLASH_BEGIN to do
// the erase (ROM aligns up to full 4 KB sectors), then FLASH_END to close
// out the session without rebooting. Designed for surgical otadata /
// NVS wipes. Does NOT reboot the receiver — caller issues reboot separately.
Result eraseRegion(const Config &cfg, uint32_t offset, size_t size) {
    if (!cfg.uart || size == 0) return FLASH_ERR_INVALID_INPUT;

    s_uart = cfg.uart;
    s_uart->setRxBufferSize(4096);
    s_uart->begin(cfg.baud_rate, SERIAL_8N1, cfg.rx_pin, cfg.tx_pin);
    delay(100);

    if (cfg.progress) cfg.progress(0, "Connecting");
    if (!sync()) { s_uart->end(); return FLASH_ERR_NO_SYNC; }
    if (cfg.progress) cfg.progress(10, "Synced");

    spiAttach();
    // FLASH_BEGIN with num_blocks=0 forces the ROM into "erase then wait".
    // We still pass a non-zero size so erase_size rounds up to >=1 sector.
    if (!flashBegin(size, offset)) {
        s_uart->end();
        return FLASH_ERR_BEGIN_FAILED;
    }
    if (cfg.progress) cfg.progress(70, "Erasing");

    // FLASH_END with reboot=false so the ROM just finalises; caller decides
    // when/if to reboot.
    if (!flashEnd(false)) {
        // Some ROMs silently don't respond — treat as best-effort.
    }

    if (cfg.progress) cfg.progress(100, "Done");
    s_uart->end();
    return FLASH_OK;
}

// Multi-region variant of eraseRegion. Syncs ONCE, runs N erase
// commands, ends ONCE. Same shape as readFlashMulti — see header for
// rationale. Use case today: routes_flash.cpp:erase_partition does a
// loop of eraseRegion() calls in 64 KB chunks; with this helper the
// loop becomes a single ROM session immune to the autobauder-latch
// issue that BUG-ID1 hit on reads.
Result eraseRegionMulti(const Config &cfg, const EraseRegion *regions, size_t n) {
    if (!cfg.uart || !regions || n == 0) return FLASH_ERR_INVALID_INPUT;
    uint32_t max_end = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t end = regions[i].offset + regions[i].size;
        if (end > max_end) max_end = end;
    }
    bool was_open = sessionOpen();
    Result r = openSession(cfg, max_end);
    if (r != FLASH_OK) return r;
    r = eraseRegionMultiInOpenSession(regions, n);
    if (!was_open) closeSession();
    return r;
}

// Read `size` bytes at `offset` into `out`.
// Uses CMD_READ_FLASH_SLOW (0x0E) — ROM-native, chunked. Each request reads
// up to 64 bytes and the data comes back inside the standard response's
// payload (not as a separate data stream). Much slower than the stub-only
// CMD_READ_FLASH (0xD2) — ~30–50 KB/s — but works on bare ROM of
// ESP32-S2/S3/C3 without needing to first upload a stub loader.
Result readFlash(const Config &cfg, uint32_t offset, size_t size, uint8_t *out) {
    if (!cfg.uart || !out || size == 0) return FLASH_ERR_INVALID_INPUT;

    s_uart = cfg.uart;
    s_uart->setRxBufferSize(8192);
    s_uart->begin(cfg.baud_rate, SERIAL_8N1, cfg.rx_pin, cfg.tx_pin);
    delay(100);

    if (cfg.progress) cfg.progress(0, "Connecting");
    if (!sync()) { s_uart->end(); return FLASH_ERR_NO_SYNC; }
    if (cfg.progress) cfg.progress(3, "Synced");

    spiAttach();
    uint32_t fs = offset + (uint32_t)size;
    if (fs < 0x400000) fs = 0x400000;
    spiSetParams(fs);

    static const uint32_t CHUNK = 64;  // max per READ_FLASH_SLOW request

    size_t received = 0;
    static uint8_t resp[128];
    while (received < size) {
        uint32_t take = (size - received > CHUNK) ? CHUNK : (size - received);

        uint8_t req[8];
        *(uint32_t*)(req + 0) = offset + (uint32_t)received;
        *(uint32_t*)(req + 4) = take;
        uint32_t ck = cksum(req, 8);

        // Issue READ_FLASH_SLOW.
        slipStart();
        slipWriteByte(0x00);
        slipWriteByte(CMD_READ_FLASH_SLOW);
        slipWriteByte(8 & 0xFF);
        slipWriteByte((8 >> 8) & 0xFF);
        slipWriteByte(ck & 0xFF);
        slipWriteByte((ck >> 8) & 0xFF);
        slipWriteByte((ck >> 16) & 0xFF);
        slipWriteByte((ck >> 24) & 0xFF);
        slipWrite(req, 8);
        slipEnd();
        s_uart->flush();

        int n = slipRead(resp, sizeof(resp), 2000);
        if (n < 8 || resp[0] != 0x01 || resp[1] != CMD_READ_FLASH_SLOW) {
            s_uart->end();
            return FLASH_ERR_READ_FAILED;
        }
        // Response format: [0x01][0xD2 echo][size_lo][size_hi]
        //                  [value:4][data...][status:2]
        // "size" field is the data length; for 0x0E it equals `take`.
        uint16_t resp_size = resp[2] | (resp[3] << 8);
        // Data starts at offset 8 (after the 4-byte value field).
        int data_off = 8;
        int data_len = (int)resp_size - 2;  // minus trailing status
        if (data_len <= 0 || data_off + data_len > n) {
            s_uart->end();
            return FLASH_ERR_READ_FAILED;
        }
        if ((uint32_t)data_len > take) data_len = (int)take;
        memcpy(out + received, resp + data_off, data_len);
        received += data_len;

        if (cfg.progress) {
            int pct = 3 + (int)((int64_t)received * 92 / size);
            cfg.progress(pct, "Reading");
        }
    }

    if (cfg.progress) cfg.progress(100, "Done");
    s_uart->end();
    return FLASH_OK;
}

// Multi-region variant of readFlash. Syncs ONCE, reads N regions, ends ONCE.
// Needed for `/api/elrs/identity/fast` (NVS + app-tail + partition-table)
// and anywhere else we need to sample several flash offsets in one DFU
// session without running into the ESP32-C3 ROM autobauder quirk.
Result readFlashMulti(const Config &cfg, const ReadRegion *regions, size_t n) {
    if (!cfg.uart || !regions || n == 0) return FLASH_ERR_INVALID_INPUT;
    uint32_t max_end = 0;
    for (size_t i = 0; i < n; i++) {
        if (!regions[i].dst || regions[i].size == 0) return FLASH_ERR_INVALID_INPUT;
        uint32_t end = regions[i].offset + regions[i].size;
        if (end > max_end) max_end = end;
    }
    bool was_open = sessionOpen();
    Result r = openSession(cfg, max_end);
    if (r != FLASH_OK) return r;
    r = readFlashMultiInOpenSession(regions, n);
    if (!was_open) closeSession();
    return r;
}

// Ask ROM/stub to compute MD5 over a flash region. Request layout:
//   [offset:u32, size:u32, zero:u32, zero:u32]  (16 bytes total)
// Response "value" field (bytes 4..7 of status reply) contains the first
// 4 bytes of the digest; the remaining 12 are in the response data area.
// We read them all by parsing the full SLIP frame.
Result spiFlashMd5(const Config &cfg, uint32_t offset, uint32_t size, uint8_t out[16]) {
    if (!cfg.uart || !out || size == 0) return FLASH_ERR_INVALID_INPUT;
    uint32_t fs = offset + size;
    if (fs < 0x400000) fs = 0x400000;
    bool was_open = sessionOpen();
    Result r = openSession(cfg, fs);
    if (r != FLASH_OK) return r;
    r = spiFlashMd5InOpenSession(offset, size, out);
    if (!was_open) closeSession();
    return r;
}

// Chip detection via READ_REG of CHIP_DETECT_MAGIC_VALUE (0x40001000) + EFUSE
// MAC registers. Magic values follow esptool.py's table — each SoC family
// writes a distinct ROM-boot sentinel into that register.
Result chipInfo(const Config &cfg, ChipInfo *out) {
    if (!cfg.uart || !out) return FLASH_ERR_INVALID_INPUT;
    bool was_open = sessionOpen();
    Result r = openSession(cfg);
    if (r != FLASH_OK) return r;
    r = chipInfoInOpenSession(out);
    if (!was_open) closeSession();
    return r;
}

// Parse ELRS build-info rodata: looks for the 4-byte `be ef ca fe` sentinel
// that binary_configurator plants before the target/version/git strings, and
// extracts the surrounding null-terminated strings. Format observed across
// vanilla ELRS 3.5.x / 3.6.x and the MILELRS fork:
//
//   <product\0> <ap_password\0> <ap_ssid\0> <hostname\0> <be ef ca fe>
//   <target\0> [<binary_name\0>] [<version\0>|<lua\0>] <git\0>
//
// Order of the "after" strings varies by build (MILELRS has MILELRS_v348 in
// the slot where vanilla has "3.5.3"), so we capture them generically and let
// the caller label them.
static void parseBuildInfo(const uint8_t *buf, size_t n, SlotIdentity *id) {
    int magic_pos = -1;
    for (size_t i = 0; i + 4 <= n; i++) {
        if (buf[i] == 0xbe && buf[i+1] == 0xef && buf[i+2] == 0xca && buf[i+3] == 0xfe) {
            magic_pos = (int)i; break;
        }
    }
    if (magic_pos < 0) return;

    // readStr: read one null-terminated printable string.
    // - Skips leading null / non-printable bytes (record separators like 0x1e
    //   that binary_configurator plants between strings break JSON).
    // - Stops on first null OR first non-printable byte OR end of buffer.
    // - Always null-terminates; returns false if nothing useful read.
    auto isPrintable = [](uint8_t c) { return c >= 0x20 && c <= 0x7e; };
    auto readStr = [&](size_t *pp, char *out, size_t max) -> bool {
        while (*pp < n && !isPrintable(buf[*pp]) && buf[*pp] != 0) (*pp)++;
        while (*pp < n && buf[*pp] == 0) (*pp)++;
        if (*pp >= n) { out[0] = 0; return false; }
        size_t start = *pp;
        while (*pp < n && isPrintable(buf[*pp]) && (*pp - start) < max - 1) {
            out[*pp - start] = (char)buf[*pp];
            (*pp)++;
        }
        out[*pp - start] = 0;
        // Advance past the remaining non-printable / null run so the next call
        // doesn't re-read the same byte.
        while (*pp < n && (buf[*pp] == 0 || !isPrintable(buf[*pp]))) (*pp)++;
        return out[0] != 0;
    };

    // After magic: target, optional binary_name, version/lua, git
    size_t p = magic_pos + 4;
    readStr(&p, id->target, sizeof(id->target));
    readStr(&p, id->version_or_lua, sizeof(id->version_or_lua));
    // Next string looks like git hash (6-8 hex chars); overwrite version if it
    // looks like a hash and previous doesn't.
    char tmp[24];
    if (readStr(&p, tmp, sizeof(tmp))) {
        auto isHex6to8 = [](const char *s) {
            size_t len = strlen(s);
            if (len < 6 || len > 8) return false;
            for (size_t i = 0; i < len; i++) {
                char c = s[i];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                    return false;
            }
            return true;
        };
        if (isHex6to8(tmp)) {
            strncpy(id->git, tmp, sizeof(id->git) - 1);
        } else if (isHex6to8(id->version_or_lua)) {
            // prev slot was actually the git hash — shift
            strncpy(id->git, id->version_or_lua, sizeof(id->git) - 1);
            strncpy(id->version_or_lua, tmp, sizeof(id->version_or_lua) - 1);
        }
    }

    // Before magic: find last null-terminated string (likely hostname "elrs_rx"
    // or the product string "ExpressLRS RX"). Walk backwards, skip null runs,
    // then pick up to 3 strings.
    int end = magic_pos - 1;
    while (end > 0 && buf[end] == 0) end--;
    // Find last string start
    int start = end;
    while (start > 0 && buf[start - 1] != 0) start--;
    // We're at start of last string ("elrs_rx" / "wifi_hostname")
    // One more step back to find the string before (usually "ExpressLRS RX")
    int prev_end = start - 1;
    while (prev_end > 0 && buf[prev_end] == 0) prev_end--;
    int prev_start = prev_end;
    while (prev_start > 0 && buf[prev_start - 1] != 0) prev_start--;

    int plen = prev_end - prev_start + 1;
    if (plen > 0 && plen < (int)sizeof(id->product)) {
        // Copy only printable ASCII to keep output JSON-safe.
        int w = 0;
        for (int i = 0; i < plen && w < (int)sizeof(id->product) - 1; i++) {
            uint8_t c = buf[prev_start + i];
            if (isPrintable(c)) id->product[w++] = (char)c;
        }
        id->product[w] = 0;
    }
}

// Scan a slot inside an already-open DFU session. Reads 16 KB at slot offset,
// marks `present` if ESP image magic 0xE9 sits at byte 0, then extracts the
// ELRS build-info strings. `first_nonff_byte` left 0 (follow-up binary search
// later — not used yet).
static bool scanSlotInSession(uint32_t slot_offset, SlotIdentity *id) {
    memset(id, 0, sizeof(*id));
    id->offset = slot_offset;

    static uint8_t buf[16384];
    if (!readFlashInSession(slot_offset, sizeof(buf), buf)) return false;

    id->present = (buf[0] == 0xE9);
    if (!id->present) return true;  // scan ok, slot just empty/corrupt
    id->entry_point = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8)
                    | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    parseBuildInfo(buf, sizeof(buf), id);
    return true;
}

// Full dual-slot receiver info in one DFU session. Sync + SPI_ATTACH + SPI_SET_PARAMS,
// then read OTADATA (0xe000+0xf000), then scan app0 and app1. Does NOT send
// FLASH_END — caller may follow up with more reads if desired.
Result receiverInfo(const Config &cfg, ReceiverInfo *out) {
    if (!cfg.uart || !out) return FLASH_ERR_INVALID_INPUT;
    memset(out, 0, sizeof(*out));
    out->active_slot = -1;
    out->slot[0].offset = 0x10000;
    out->slot[1].offset = 0x1f0000;

    s_uart = cfg.uart;
    s_uart->setRxBufferSize(8192);
    s_uart->begin(cfg.baud_rate, SERIAL_8N1, cfg.rx_pin, cfg.tx_pin);
    delay(50);

    if (!sync()) { s_uart->end(); return FLASH_ERR_NO_SYNC; }

    // Chip identity via READ_REG — same as chipInfo() but inline.
    uint32_t magic = 0;
    uint8_t req[4];
    *(uint32_t*)req = 0x40001000;
    if (sendCmd(CMD_READ_REG, req, 4, cksum(req, 4), 3000, &magic)) {
        out->chip_ok = true;
        out->chip.magic_value = magic;
        out->chip.ok = true;
        switch (magic) {
            case 0xfff0c101: out->chip.chip_name = "ESP8266"; break;
            case 0x00f01d83: out->chip.chip_name = "ESP32"; break;
            case 0x000007c6: out->chip.chip_name = "ESP32-S2"; break;
            case 0x00000009:
            case 0xeb004136: out->chip.chip_name = "ESP32-S3"; break;
            case 0x6921506f:
            case 0x1b31506f: out->chip.chip_name = "ESP32-C3"; break;
            case 0x0da1806f: out->chip.chip_name = "ESP32-C6"; break;
            case 0xd7b73e80: out->chip.chip_name = "ESP32-H2"; break;
            default:         out->chip.chip_name = "unknown"; break;
        }
        if (magic == 0x6921506f || magic == 0x1b31506f) {
            uint32_t m0 = 0, m1 = 0;
            *(uint32_t*)req = 0x60008844;
            sendCmd(CMD_READ_REG, req, 4, cksum(req, 4), 3000, &m0);
            *(uint32_t*)req = 0x60008848;
            sendCmd(CMD_READ_REG, req, 4, cksum(req, 4), 3000, &m1);
            out->chip.mac[0] = (uint8_t)(m1 >> 8);
            out->chip.mac[1] = (uint8_t)(m1 >> 0);
            out->chip.mac[2] = (uint8_t)(m0 >> 24);
            out->chip.mac[3] = (uint8_t)(m0 >> 16);
            out->chip.mac[4] = (uint8_t)(m0 >> 8);
            out->chip.mac[5] = (uint8_t)(m0 >> 0);
        }
    }

    // Prep flash subsystem once, then do multiple reads in-session.
    spiAttach();
    spiSetParams(0x400000);

    // OTADATA — 2 × 32 B at 0xe000 and 0xf000.
    uint8_t otab[2][32];
    bool ota_ok[2] = {false, false};
    ota_ok[0] = readFlashInSession(0xe000, 32, otab[0]);
    ota_ok[1] = readFlashInSession(0xf000, 32, otab[1]);
    out->otadata_ok = (ota_ok[0] || ota_ok[1]);
    uint32_t max_seq = 0;
    for (int i = 0; i < 2; i++) {
        out->otadata[i].read_ok = ota_ok[i];
        if (!ota_ok[i]) continue;
        uint32_t seq   = *(uint32_t*)&otab[i][0];
        uint32_t state = *(uint32_t*)&otab[i][24];
        uint32_t crc   = *(uint32_t*)&otab[i][28];
        out->otadata[i].seq = seq;
        out->otadata[i].state = state;
        out->otadata[i].crc = crc;
        out->otadata[i].blank = (seq == 0xFFFFFFFF);
        if (!out->otadata[i].blank && seq > max_seq) max_seq = seq;
    }
    out->max_seq = max_seq;
    out->active_slot = (max_seq == 0) ? -1 : (int)((max_seq - 1) & 1);

    // Slot scan — app0 / app1.
    scanSlotInSession(0x10000,  &out->slot[0]);
    scanSlotInSession(0x1f0000, &out->slot[1]);

    s_uart->end();
    return FLASH_OK;
}

// CRSF 'bl' frame — the authoritative 6-byte command that puts an ELRS
// receiver into its bootloader/stub flasher. On ESP32-C3 this reaches the
// in-app stub (not ROM DFU) at whatever baud the frame arrived on — so
// cfg.baud_rate MUST be 420000 (vanilla CRSF default). TX-only.
// Frame: EC 04 32 62 6C 0A (last byte = CRC8-DVB-S2 poly 0xD5 over bytes
// [type..payload]). Verified against
// hardware/bayckrc_c3_dual/elrs_3_6_3_src/src/test/test_telemetry/test_telemetry.cpp.
Result sendCrsfReboot(const Config &cfg) {
    if (!cfg.uart) return FLASH_ERR_INVALID_INPUT;
    s_uart = cfg.uart;
    s_uart->setRxBufferSize(512);
    s_uart->begin(cfg.baud_rate, SERIAL_8N1, cfg.rx_pin, cfg.tx_pin);
    delay(20);
    static const uint8_t frame[6] = {0xEC, 0x04, 0x32, 0x62, 0x6C, 0x0A};
    s_uart->write(frame, sizeof(frame));
    s_uart->flush();
    // Give the ELRS telemetry task a slot to consume + flip into serialUpdate.
    // Measured: ~150 ms covers "bl received → stub ready to answer SYNC" on
    // a running vanilla 3.5.3 image. We wait a bit longer for safety.
    delay(250);
    s_uart->end();
    return FLASH_OK;
}

// OTADATA read + write within a single Serial1 session. Two separate
// readFlash + flash calls glitch the ROM autobauder-lock between end/begin
// cycles — the write's SYNC then fails with "No sync" despite the read
// having just succeeded. This helper sends sync + spi_attach + spi_set_params
// ONCE, reads both 32 B OTADATA sectors via READ_FLASH_SLOW, computes the
// new ota_seq for the desired slot, then does FLASH_BEGIN + FLASH_DATA +
// FLASH_END on the lower-seq sector — all without closing Serial1.
//
// On ESP32-C3, FLASH_END with reboot=1 makes the ROM reboot into app. The
// caller gets the chosen seq / sector back for logging.
Result otadataSelect(const Config &cfg, int desired_slot,
                     uint32_t *out_new_seq, uint32_t *out_target_offset) {
    if (!cfg.uart || (desired_slot != 0 && desired_slot != 1)) return FLASH_ERR_INVALID_INPUT;

    s_uart = cfg.uart;
    s_uart->setRxBufferSize(4096);
    s_uart->begin(cfg.baud_rate, SERIAL_8N1, cfg.rx_pin, cfg.tx_pin);
    delay(100);

    if (!sync()) { s_uart->end(); return FLASH_ERR_NO_SYNC; }
    spiAttach();
    spiSetParams(0x400000);

    // Read both sectors in-session.
    uint8_t sec0[32] = {0}, sec1[32] = {0};
    bool ok0 = readFlashInSession(0xe000, 32, sec0);
    bool ok1 = readFlashInSession(0xf000, 32, sec1);
    if (!ok0 && !ok1) { s_uart->end(); return FLASH_ERR_READ_FAILED; }

    uint32_t seq0 = ok0 ? *(uint32_t*)&sec0[0] : 0xFFFFFFFF;
    uint32_t seq1 = ok1 ? *(uint32_t*)&sec1[0] : 0xFFFFFFFF;
    uint32_t max_seq = 0;
    if (seq0 != 0xFFFFFFFF) max_seq = seq0;
    if (seq1 != 0xFFFFFFFF && seq1 > max_seq) max_seq = seq1;

    // Choose new_seq so that (new_seq-1)&1 == desired_slot. Bump by 1 or 2.
    uint32_t new_seq = max_seq + 1;
    int cur = (int)((new_seq - 1) & 1);
    if (cur != desired_slot) new_seq++;
    if (new_seq == 0 || new_seq > 0x7fffffff) { s_uart->end(); return FLASH_ERR_INVALID_INPUT; }

    // Build 32 B OTADATA record: [seq:4][label:20 zeros][state:4=0xffffffff][crc32:4]
    uint8_t rec[32];
    memset(rec, 0, 32);
    *(uint32_t*)&rec[0]  = new_seq;
    *(uint32_t*)&rec[24] = 0xFFFFFFFF;
    uint32_t crc = 0xFFFFFFFF;
    for (int j = 0; j < 4; j++) {
        crc ^= rec[j];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
    }
    crc ^= 0xFFFFFFFF;
    *(uint32_t*)&rec[28] = crc;

    // Write to the sector with the LOWER seq (alternating-sector spec).
    uint32_t target_offset = (seq0 <= seq1) ? 0xe000 : 0xf000;
    if (out_new_seq)       *out_new_seq       = new_seq;
    if (out_target_offset) *out_target_offset = target_offset;

    // FLASH_BEGIN for 32 bytes at target_offset. Then one FLASH_DATA block
    // (32 bytes, padded to 4 KB with 0xFF), then FLASH_END(reboot=1).
    if (!flashBegin(32, target_offset)) { s_uart->end(); return FLASH_ERR_BEGIN_FAILED; }
    if (!flashData(rec, 32, 0))         { s_uart->end(); return FLASH_ERR_WRITE_FAILED; }
    // reboot=true so RX boots into the newly-selected app partition.
    flashEnd(true);

    s_uart->end();
    return FLASH_OK;
}

// Shared CRSF CRC8-DVB-S2 (poly 0xD5, init 0) over [data..data+n).
static uint8_t crsfCrc8(const uint8_t *data, size_t n) {
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= data[i];
        for (int b = 0; b < 8; b++) c = (c & 0x80) ? ((c << 1) ^ 0xD5) : (c << 1);
    }
    return c;
}

// DEVICE_PING (CRSF ext-header type 0x28) → DEVICE_INFO (0x29) parse.
// Frame layout (`src/lib/CrsfProtocol/crsf_protocol.h` + CRSFEndpoint):
//   TX: EC 04 28 00 EC <crc>  (broadcast ping)
//   RX: <addr> <len> 29 <dest=EC> <orig=EC> <name\0> <ser_u32_BE>
//       <hw_u32_BE> <sw_u32_BE> <field_cnt> <param_ver> <crc>
Result crsfDevicePing(const Config &cfg, uint32_t timeout_ms, ElrsDeviceInfo *out) {
    if (!cfg.uart || !out) return FLASH_ERR_INVALID_INPUT;
    memset(out, 0, sizeof(*out));

    s_uart = cfg.uart;
    s_uart->setRxBufferSize(512);
    s_uart->begin(cfg.baud_rate, SERIAL_8N1, cfg.rx_pin, cfg.tx_pin);
    delay(20);
    while (s_uart->available()) s_uart->read();

    // TX ping. orig=0xEA matches elrsV3.lua handset-to-receiver convention
    // (line 386: handsetId = 0xEA for non-TX devices). Was 0xEC earlier —
    // the RX's dispatch path didn't care about orig, but its DEVICE_INFO
    // reply chain may: fix now to match reference.
    auto sendPing = [&]() {
        uint8_t ping[6] = {0xEC, 0x04, 0x28, 0x00, 0xEA, 0x00};
        ping[5] = crsfCrc8(ping + 2, 3);
        while (s_uart->available()) s_uart->read();
        s_uart->write(ping, 6);
        s_uart->flush();
    };
    sendPing();

    // RX: scan incoming bytes for `<addr> <len> 29 …<crc>` within timeout.
    // If nothing arrives by timeout/2, re-send ping (LUA does this at ~100 ms
    // cadence during device discovery). Triples our chance of hitting a
    // receiver that was mid-something when the first frame arrived.
    uint8_t buf[128];
    size_t got = 0;
    uint32_t deadline = millis() + timeout_ms;
    uint32_t half = millis() + timeout_ms / 2;
    bool resent = false;
    while (millis() < deadline && got < sizeof(buf)) {
        while (s_uart->available() && got < sizeof(buf)) buf[got++] = s_uart->read();
        if (!resent && got == 0 && millis() >= half) { sendPing(); resent = true; }
        delay(2);
    }
    s_uart->end();

    // Find the 0x29 DEVICE_INFO response.
    int found = -1;
    for (size_t i = 0; i + 3 < got; i++) {
        uint8_t len = buf[i + 1];
        if (buf[i + 2] == 0x29 && len >= 12 && i + 2 + len <= got) {
            found = (int)i;
            break;
        }
    }
    if (found < 0) return FLASH_ERR_READ_FAILED;

    uint8_t plen = buf[found + 1];
    // Body: [type=29][dest][orig][name\0...][ser:4][hw:4][sw:4][fcnt][pver][crc]
    const uint8_t *body = buf + found + 3;  // skip [addr,len,type]
    int body_len = plen - 2;                // minus type+crc

    // Name starts at body[2] (after dest+orig), null-terminated.
    int name_start = 2;
    int name_end = name_start;
    while (name_end < body_len && body[name_end] != 0) name_end++;
    int nlen = name_end - name_start;
    if (nlen > (int)sizeof(out->name) - 1) nlen = sizeof(out->name) - 1;
    memcpy(out->name, body + name_start, nlen);
    out->name[nlen] = 0;

    // After name\0: 4+4+4 = 12 bytes of BE-encoded u32 + field_cnt + parameter_ver.
    int after = name_end + 1;
    if (after + 14 > body_len) return FLASH_ERR_READ_FAILED;
    auto be32 = [](const uint8_t *p) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    };
    out->serial_no         = be32(body + after);
    out->hw_id             = be32(body + after + 4);
    out->sw_version        = be32(body + after + 8);
    out->field_count       = body[after + 12];
    out->parameter_version = body[after + 13];
    out->ok = true;
    return FLASH_OK;
}

// LUA parameter read — single chunk.
// Frame layout:
//   TX: EC 06 2C EC EA <field_id> <chunk> <crc>   (8 bytes)
//   RX: <addr> <len> 2B <dest=EC> <orig=EC> <field_id> <chunks_remaining>
//       <parent> <type> <name\0> <type-specific> <crc>
// We return out_buf[] starting at <parent> (skipping field_id+chunks_remaining).
Result crsfParamRead(const Config &cfg, uint8_t field_id, uint8_t chunk,
                     uint8_t *out_buf, size_t max_bytes, size_t *out_len,
                     uint8_t *chunks_remaining) {
    if (!cfg.uart || !out_buf || !out_len) return FLASH_ERR_INVALID_INPUT;
    *out_len = 0;
    if (chunks_remaining) *chunks_remaining = 0;

    s_uart = cfg.uart;
    s_uart->setRxBufferSize(1024);
    s_uart->begin(cfg.baud_rate, SERIAL_8N1, cfg.rx_pin, cfg.tx_pin);
    delay(15);
    while (s_uart->available()) s_uart->read();

    // Send request. elrsV3.lua uses handsetId=0xEA for non-TX devices (line 386)
    // and sends at a 500 ms cadence per param for remote devices (line 575). So
    // our 400 ms window with one mid-deadline resend tracks that pattern.
    auto sendReq = [&]() {
        uint8_t req[8] = {0xEC, 0x06, 0x2C, 0xEC, 0xEA, field_id, chunk, 0x00};
        req[7] = crsfCrc8(req + 2, 5);
        while (s_uart->available()) s_uart->read();
        s_uart->write(req, 8);
        s_uart->flush();
    };
    sendReq();

    // Collect up to 256 bytes from wire — enough for 60-byte frame + safety.
    uint8_t buf[256];
    size_t got = 0;
    uint32_t deadline = millis() + 400;
    uint32_t half     = millis() + 200;
    bool resent = false;
    while (millis() < deadline && got < sizeof(buf)) {
        while (s_uart->available() && got < sizeof(buf)) buf[got++] = s_uart->read();
        if (!resent && got == 0 && millis() >= half) { sendReq(); resent = true; }
        delay(2);
    }
    s_uart->end();

    // Scan for: <addr> <len> 0x2B <...> — first 0x2B frame.
    for (size_t i = 0; i + 4 < got; i++) {
        uint8_t len = buf[i + 1];
        if (buf[i + 2] != 0x2B) continue;
        if (len < 6 || i + 2 + len > got) continue;
        // body starts at buf[i+3]: [dest][orig][field_id][chunks_rem][parent][type][name...]
        // skip [dest=i+3] [orig=i+4]
        uint8_t reply_field = buf[i + 5];
        if (reply_field != field_id) continue;
        if (chunks_remaining) *chunks_remaining = buf[i + 6];
        // body after field_id+chunks_rem: buf[i+7..i+2+len-1] (last byte is crc).
        int body_start = i + 7;
        int body_end = i + 2 + len - 1;  // excl. crc
        if (body_end <= body_start) return FLASH_ERR_READ_FAILED;
        size_t copy_n = body_end - body_start;
        if (copy_n > max_bytes) copy_n = max_bytes;
        memcpy(out_buf, buf + body_start, copy_n);
        *out_len = copy_n;
        return FLASH_OK;
    }
    return FLASH_ERR_READ_FAILED;
}

// LUA parameter write — no reply expected. Frame:
//   EC <len> 2D EC EA <field_id> <data...> <crc>
//   len = 1(type) + 1(dest) + 1(orig) + 1(field) + data_len + 1(crc) = 5 + data_len
Result crsfParamWrite(const Config &cfg, uint8_t field_id, const uint8_t *data, uint8_t data_len) {
    if (!cfg.uart || data_len > 32) return FLASH_ERR_INVALID_INPUT;
    s_uart = cfg.uart;
    s_uart->setRxBufferSize(256);
    s_uart->begin(cfg.baud_rate, SERIAL_8N1, cfg.rx_pin, cfg.tx_pin);
    delay(15);

    uint8_t frame[48];
    frame[0] = 0xEC;
    frame[1] = 5 + data_len;    // type+dest+orig+field_id+data+crc
    frame[2] = 0x2D;
    frame[3] = 0xEC;
    frame[4] = 0xEA;
    frame[5] = field_id;
    if (data && data_len) memcpy(frame + 6, data, data_len);
    size_t total = 6 + data_len;
    frame[total] = crsfCrc8(frame + 2, total - 2);
    s_uart->write(frame, total + 1);
    s_uart->flush();
    delay(80);
    s_uart->end();
    return FLASH_OK;
}

// CRSF "enter binding" — EC 04 32 62 64 <crc> at cfg.baud_rate.
Result sendCrsfBind(const Config &cfg) {
    if (!cfg.uart) return FLASH_ERR_INVALID_INPUT;
    s_uart = cfg.uart;
    s_uart->setRxBufferSize(256);
    s_uart->begin(cfg.baud_rate, SERIAL_8N1, cfg.rx_pin, cfg.tx_pin);
    delay(20);
    uint8_t frame[6] = {0xEC, 0x04, 0x32, 0x62, 0x64, 0x00};
    frame[5] = crsfCrc8(frame + 2, 3);
    s_uart->write(frame, sizeof(frame));
    s_uart->flush();
    delay(100);
    s_uart->end();
    return FLASH_OK;
}

// Exit stub/ROM, jump to user code (i.e. OTADATA-selected app).
// Stub implementation in ELRS calls ESP.restart(); ROM bootloader jumps to
// app directly. Opcode: 0xD3, no args, no response (stub may or may not
// reply — we don't wait).
Result runUserCode(const Config &cfg) {
    if (!cfg.uart) return FLASH_ERR_INVALID_INPUT;
    s_uart = cfg.uart;
    s_uart->setRxBufferSize(1024);
    s_uart->begin(cfg.baud_rate, SERIAL_8N1, cfg.rx_pin, cfg.tx_pin);
    delay(50);
    // Sync first to make sure we're talking to the stub/ROM (some ROMs
    // won't accept commands until they've seen at least one valid frame).
    if (!sync()) { s_uart->end(); return FLASH_ERR_NO_SYNC; }
    // RUN_USER_CODE payload is 0 bytes.
    slipStart();
    slipWriteByte(0x00);              // dir = request
    slipWriteByte(CMD_RUN_USER_CODE); // 0xD3
    slipWriteByte(0x00); slipWriteByte(0x00);  // size = 0
    slipWriteByte(0); slipWriteByte(0); slipWriteByte(0); slipWriteByte(0);  // cksum = 0
    slipEnd();
    s_uart->flush();
    // The stub runs ESP.restart() — UART silent after. Don't wait.
    delay(200);
    s_uart->end();
    return FLASH_OK;
}

} // namespace ESPFlasher
