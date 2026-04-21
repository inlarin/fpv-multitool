#include "esp_rom_flasher.h"

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

// Main flash entry point.
// Large writes (>~256 KB) on ESP32-C3 ROM drop silently past a certain
// point within a single FLASH_BEGIN session — empirically 288 KB on the
// Bayck RC C3 Dual. Workaround: split the payload into CHUNK_SIZE
// sessions, each with its own FLASH_BEGIN / DATA... / FLASH_END(stay).
// Only the FINAL FLASH_END respects cfg.stay_in_loader.
Result flash(const Config &cfg, const uint8_t *data, size_t size) {
    if (!cfg.uart || !data || size == 0) return FLASH_ERR_INVALID_INPUT;

    s_uart = cfg.uart;
    // setRxBufferSize MUST be called before begin() — Arduino-ESP32
    // allocates the UART ring during begin() and ignores later resizes
    // until the next end()/begin() cycle. A stuck-at-256 ring overflows
    // mid-frame at 115200 baud and desyncs the SLIP parser.
    s_uart->setRxBufferSize(4096);
    s_uart->begin(cfg.baud_rate, SERIAL_8N1, cfg.rx_pin, cfg.tx_pin);
    delay(100);

    if (cfg.progress) cfg.progress(0, "Connecting");

    // SYNC
    if (!sync()) {
        s_uart->end();
        return FLASH_ERR_NO_SYNC;
    }
    if (cfg.progress) cfg.progress(5, "Synced");

    // SPI_ATTACH — idempotent, do once.
    spiAttach();
    uint32_t fs = cfg.flash_offset + (uint32_t)size;
    if (fs < 0x400000) fs = 0x400000;
    spiSetParams(fs);

    // Split into ≤CHUNK_SIZE chunks to avoid the >288 KB silent-drop.
    const size_t CHUNK_SIZE = 0x40000;   // 256 KB — safely under the cliff
    size_t written_so_far = 0;
    while (written_so_far < size) {
        size_t chunk_len = size - written_so_far;
        if (chunk_len > CHUNK_SIZE) chunk_len = CHUNK_SIZE;
        bool is_last_chunk = (written_so_far + chunk_len >= size);

        if (!flashBegin(chunk_len, cfg.flash_offset + written_so_far)) {
            s_uart->end();
            return FLASH_ERR_BEGIN_FAILED;
        }
        if (cfg.progress) cfg.progress(10 + (int)(written_so_far * 85 / size), "Erasing");

        uint32_t num_blocks = (chunk_len + FLASH_WRITE_BLOCK_SIZE - 1) / FLASH_WRITE_BLOCK_SIZE;
        for (uint32_t i = 0; i < num_blocks; i++) {
            size_t chunk_off = i * FLASH_WRITE_BLOCK_SIZE;
            size_t block_size = min((size_t)FLASH_WRITE_BLOCK_SIZE, chunk_len - chunk_off);
            if (!flashData(data + written_so_far + chunk_off, block_size, i)) {
                s_uart->end();
                return FLASH_ERR_WRITE_FAILED;
            }
            // Throttle — ESP32-C3 ROM's internal flash-write queue acks
            // FLASH_DATA before the write is actually committed. Sending
            // next block too fast causes silent drops past ~288 blocks.
            delayMicroseconds(5000);
            if (cfg.progress) {
                int pct = 10 + (int)((written_so_far + chunk_off + block_size) * 85 / size);
                cfg.progress(pct, "Writing");
            }
        }

        // End-of-chunk FLASH_END with stay-in-loader so ROM doesn't reset
        // between chunks. Only the final chunk may reboot (see below).
        if (!is_last_chunk) {
            flashEnd(false);  // false = stay in bootloader between chunks
        }
        written_so_far += chunk_len;
    }

    // FLASH_END — only send when we actually want to reboot. Some ESP32-C3
    // ROM variants auto-reset on ANY FLASH_END regardless of the payload
    // flag, so for chained operations we just don't send it at all.
    if (!cfg.stay_in_loader) {
        if (!flashEnd(true)) {
            // Non-fatal: some bootloaders don't respond to FLASH_END.
        }
    }

    if (cfg.progress) cfg.progress(100, "Done");
    s_uart->end();
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

// Ask ROM/stub to compute MD5 over a flash region. Request layout:
//   [offset:u32, size:u32, zero:u32, zero:u32]  (16 bytes total)
// Response "value" field (bytes 4..7 of status reply) contains the first
// 4 bytes of the digest; the remaining 12 are in the response data area.
// We read them all by parsing the full SLIP frame.
Result spiFlashMd5(const Config &cfg, uint32_t offset, uint32_t size, uint8_t out[16]) {
    if (!cfg.uart || !out || size == 0) return FLASH_ERR_INVALID_INPUT;
    s_uart = cfg.uart;
    s_uart->setRxBufferSize(1024);
    s_uart->begin(cfg.baud_rate, SERIAL_8N1, cfg.rx_pin, cfg.tx_pin);
    delay(50);
    if (!sync()) { s_uart->end(); return FLASH_ERR_NO_SYNC; }

    // ESP32-C3 ROM's SPI_FLASH_MD5 needs the SPI peripheral attached and
    // geometry declared — same prerequisites as READ_FLASH. Without them the
    // ROM rejects the command with status=1 (symptom: "READ_FLASH rejected").
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

    // Response: dir=0x01, cmd=0x13, size=... varies per ROM variant:
    //   * ROM reply: [dir=1, cmd=0x13, size=2+16+2, value=0, payload=<32 ASCII hex chars>+status_word]
    //     — ROM sends the MD5 as 32 hex chars!
    //   * Stub reply: [dir=1, cmd=0x13, size=2+16, value=0, payload=<16 raw bytes>+status_word]
    // We accept both and convert ASCII-hex to raw if needed.
    static uint8_t buf[128];
    int n = slipRead(buf, sizeof(buf), 10000);  // MD5 over 1 MB ~200 ms but give margin
    if (n < 8 || buf[0] != 0x01 || buf[1] != CMD_SPI_FLASH_MD5) {
        s_uart->end();
        return FLASH_ERR_READ_FAILED;
    }
    uint16_t resp_size = buf[2] | (buf[3] << 8);
    if (n < 8 + resp_size) { s_uart->end(); return FLASH_ERR_READ_FAILED; }

    // Status is last 2 bytes of the data area.
    uint8_t status = buf[8 + resp_size - 2];
    if (status != 0x00) { s_uart->end(); return FLASH_ERR_READ_FAILED; }

    // The 16 MD5 bytes (raw or ASCII-hex) sit at buf[8 .. 8 + resp_size - 2].
    uint8_t *md5_area = &buf[8];
    uint16_t md5_len = resp_size - 2;

    if (md5_len == 16) {
        memcpy(out, md5_area, 16);
    } else if (md5_len == 32) {
        // ASCII hex from ROM — convert
        for (int i = 0; i < 16; i++) {
            char hi = md5_area[i * 2];
            char lo = md5_area[i * 2 + 1];
            auto h2i = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            out[i] = (uint8_t)((h2i(hi) << 4) | h2i(lo));
        }
    } else {
        s_uart->end();
        return FLASH_ERR_READ_FAILED;
    }

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
