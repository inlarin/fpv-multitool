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
static const uint8_t CMD_SPI_ATTACH   = 0x0D;
static const uint8_t CMD_CHANGE_BAUD  = 0x0F;
static const uint8_t CMD_READ_FLASH   = 0xD2;  // stream-read (ESP32-S2/S3/C3 ROM + stubs)

// Parameters
static const size_t FLASH_WRITE_BLOCK_SIZE = 1024;    // per FLASH_DATA packet
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

    // Last 2 bytes = status
    if (resp_size < 2 || n < 8 + resp_size) return true; // some commands have no status
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

// Erase region and prepare for writing
static bool flashBegin(uint32_t size, uint32_t offset) {
    uint32_t block_size = FLASH_WRITE_BLOCK_SIZE;
    uint32_t num_blocks = (size + block_size - 1) / block_size;
    uint32_t erase_size = num_blocks * block_size;

    uint8_t data[16];
    *(uint32_t*)(data + 0) = erase_size;
    *(uint32_t*)(data + 4) = num_blocks;
    *(uint32_t*)(data + 8) = block_size;
    *(uint32_t*)(data + 12) = offset;

    uint32_t ck = cksum(data, 16);
    return sendCmd(CMD_FLASH_BEGIN, data, 16, ck, 10000); // erase may take long
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

// Main flash entry point
Result flash(const Config &cfg, const uint8_t *data, size_t size) {
    if (!cfg.uart || !data || size == 0) return FLASH_ERR_INVALID_INPUT;

    s_uart = cfg.uart;
    s_uart->begin(cfg.baud_rate, SERIAL_8N1, cfg.rx_pin, cfg.tx_pin);
    s_uart->setRxBufferSize(4096);
    delay(100);

    if (cfg.progress) cfg.progress(0, "Connecting");

    // SYNC
    if (!sync()) {
        s_uart->end();
        return FLASH_ERR_NO_SYNC;
    }
    if (cfg.progress) cfg.progress(5, "Synced");

    // FLASH_BEGIN
    if (!flashBegin(size, cfg.flash_offset)) {
        s_uart->end();
        return FLASH_ERR_BEGIN_FAILED;
    }
    if (cfg.progress) cfg.progress(10, "Erasing");

    // FLASH_DATA blocks
    uint32_t num_blocks = (size + FLASH_WRITE_BLOCK_SIZE - 1) / FLASH_WRITE_BLOCK_SIZE;
    for (uint32_t i = 0; i < num_blocks; i++) {
        size_t offset = i * FLASH_WRITE_BLOCK_SIZE;
        size_t block_size = min((size_t)FLASH_WRITE_BLOCK_SIZE, size - offset);

        if (!flashData(data + offset, block_size, i)) {
            s_uart->end();
            return FLASH_ERR_WRITE_FAILED;
        }

        if (cfg.progress) {
            int pct = 10 + (int)((i + 1) * 85 / num_blocks);
            cfg.progress(pct, "Writing");
        }
    }

    // FLASH_END with reboot
    if (!flashEnd(true)) {
        // Non-fatal: some bootloaders don't respond to FLASH_END
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

// Read `size` bytes at `offset` into `out`. Protocol per esptool's
// esp_loader_flash_read: send READ_FLASH request, then for each `block_size`
// chunk the ROM pushes a raw SLIP frame of block_size bytes, after each we
// send a 4-byte SLIP-framed ACK with total bytes received so far. Finally
// the ROM sends a 16-byte MD5 digest we discard.
Result readFlash(const Config &cfg, uint32_t offset, size_t size, uint8_t *out) {
    if (!cfg.uart || !out || size == 0) return FLASH_ERR_INVALID_INPUT;

    s_uart = cfg.uart;
    s_uart->begin(cfg.baud_rate, SERIAL_8N1, cfg.rx_pin, cfg.tx_pin);
    s_uart->setRxBufferSize(8192);
    delay(100);

    if (cfg.progress) cfg.progress(0, "Connecting");
    if (!sync()) { s_uart->end(); return FLASH_ERR_NO_SYNC; }
    if (cfg.progress) cfg.progress(3, "Synced");

    // SPI_ATTACH — optional on some ROMs, mandatory on ESP32-C3.
    spiAttach();

    const uint32_t BLOCK = 1024;
    const uint32_t MAX_PENDING = 1;  // conservative — one outstanding packet

    uint8_t req[16];
    *(uint32_t*)(req + 0)  = offset;
    *(uint32_t*)(req + 4)  = (uint32_t)size;
    *(uint32_t*)(req + 8)  = BLOCK;
    *(uint32_t*)(req + 12) = MAX_PENDING;

    // Issue READ_FLASH — the ROM's *immediate* response is a plain status
    // packet (0x01, 0xD2, ...). We don't use sendCmd() here because it
    // consumes the buffered sync replies and we'd miss the data stream.
    slipStart();
    slipWriteByte(0x00);
    slipWriteByte(CMD_READ_FLASH);
    slipWriteByte(16 & 0xFF);
    slipWriteByte((16 >> 8) & 0xFF);
    uint32_t ck = cksum(req, 16);
    slipWriteByte(ck & 0xFF);
    slipWriteByte((ck >> 8) & 0xFF);
    slipWriteByte((ck >> 16) & 0xFF);
    slipWriteByte((ck >> 24) & 0xFF);
    slipWrite(req, 16);
    slipEnd();
    s_uart->flush();

    // Consume the acknowledgement response (variable length, ends with 0xC0)
    static uint8_t tmp[256];
    int ackLen = slipRead(tmp, sizeof(tmp), 5000);
    if (ackLen < 8 || tmp[0] != 0x01 || tmp[1] != CMD_READ_FLASH) {
        s_uart->end();
        return FLASH_ERR_READ_FAILED;
    }

    // Receive data stream — one SLIP frame per block of BLOCK bytes,
    // except the last which may be shorter when size isn't block-aligned.
    size_t received = 0;
    while (received < size) {
        int n = slipRead(out + received,
                         (size - received > BLOCK) ? BLOCK : (size - received),
                         5000);
        if (n <= 0) { s_uart->end(); return FLASH_ERR_READ_FAILED; }
        received += n;

        // ACK: 4-byte little-endian count of bytes received so far.
        uint32_t ackVal = (uint32_t)received;
        slipStart();
        slipWriteByte(ackVal & 0xFF);
        slipWriteByte((ackVal >> 8) & 0xFF);
        slipWriteByte((ackVal >> 16) & 0xFF);
        slipWriteByte((ackVal >> 24) & 0xFF);
        slipEnd();
        s_uart->flush();

        if (cfg.progress) {
            int pct = 3 + (int)((int64_t)received * 92 / size);
            cfg.progress(pct, "Reading");
        }
    }

    // Trailer: MD5 digest (16 bytes in a SLIP frame). Discard.
    slipRead(tmp, sizeof(tmp), 3000);

    if (cfg.progress) cfg.progress(100, "Done");
    s_uart->end();
    return FLASH_OK;
}

} // namespace ESPFlasher
