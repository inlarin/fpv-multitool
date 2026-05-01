#include "smbus.h"

// Use Wire1 (secondary I2C bus) to avoid conflict with QMI8658 IMU on Wire (GPIO 48/47).
// Battery SMBus on free header pins GPIO 11 (SDA) + GPIO 12 (SCL).
static TwoWire *s_wire = &Wire1;

static uint8_t s_sda = 0, s_scl = 0;
static SemaphoreHandle_t s_busMutex = nullptr;

SemaphoreHandle_t SMBus::busMutex() {
    if (!s_busMutex) s_busMutex = xSemaphoreCreateMutex();
    return s_busMutex;
}
bool SMBus::busLock(uint32_t timeout_ms) {
    if (!s_busMutex) s_busMutex = xSemaphoreCreateMutex();
    return xSemaphoreTake(s_busMutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
void SMBus::busUnlock() {
    if (s_busMutex) xSemaphoreGive(s_busMutex);
}

// ===== Transaction log ring buffer =====
static SMBus::LogEntry s_log[SMBus::LOG_SIZE];
static volatile int s_logHead = 0;
static volatile uint32_t s_logSeq = 0;
static bool s_logOn = false;

static void logPush(SMBus::LogOp op, uint8_t addr, uint8_t reg, bool ok, int16_t len, const uint8_t *data) {
    if (!s_logOn) return;
    auto &e = s_log[s_logHead % SMBus::LOG_SIZE];
    e.ts   = millis();
    e.op   = op;
    e.addr = addr;
    e.reg  = reg;
    e.ok   = ok;
    e.len  = len;
    int n = (len > 0 && len < 8) ? len : (len >= 8 ? 8 : 0);
    if (data && n > 0) memcpy(e.data, data, n);
    else memset(e.data, 0, 8);
    s_logHead++;
    s_logSeq++;
}

uint32_t SMBus::logSeq()        { return s_logSeq; }
void     SMBus::logEnable(bool on) { s_logOn = on; }
bool     SMBus::logEnabled()    { return s_logOn; }

int SMBus::logDump(LogEntry *out, int max) {
    int total = s_logHead < LOG_SIZE ? s_logHead : LOG_SIZE;
    int start = s_logHead < LOG_SIZE ? 0 : s_logHead % LOG_SIZE;
    int n = total < max ? total : max;
    for (int i = 0; i < n; i++) {
        out[i] = s_log[(start + (total - n) + i) % LOG_SIZE];
    }
    return n;
}

// 9-clock SCL recovery: if SDA is stuck low (slave holding it), toggle SCL
// up to 9 times to force slave to release. Standard I2C recovery technique.
static void i2cRecover() {
    pinMode(s_scl, OUTPUT);
    for (int i = 0; i < 9; i++) {
        digitalWrite(s_scl, LOW);  delayMicroseconds(5);
        digitalWrite(s_scl, HIGH); delayMicroseconds(5);
        if (digitalRead(s_sda) == HIGH) break;
    }
    // STOP condition
    pinMode(s_sda, OUTPUT);
    digitalWrite(s_sda, LOW);  delayMicroseconds(5);
    digitalWrite(s_scl, HIGH); delayMicroseconds(5);
    digitalWrite(s_sda, HIGH); delayMicroseconds(5);
    // Restore pullups
    pinMode(s_sda, INPUT_PULLUP);
    pinMode(s_scl, INPUT_PULLUP);
}

void SMBus::init(uint8_t sda, uint8_t scl) {
    s_sda = sda; s_scl = scl;
    pinMode(sda, INPUT_PULLUP);
    pinMode(scl, INPUT_PULLUP);
    // If SDA is stuck low from a previous crash, recover first
    if (digitalRead(sda) == LOW) i2cRecover();
    s_wire->begin(sda, scl);
    s_wire->setClock(100000);
    s_wire->setTimeOut(50);
}

bool SMBus::devicePresent(uint8_t addr) {
    s_wire->beginTransmission(addr);
    return s_wire->endTransmission() == 0;
}

// Wait for N bytes in RX FIFO with timeout
static bool waitBytes(TwoWire *w, int n, uint32_t timeout_ms) {
    uint32_t start = millis();
    while (w->available() < n) {
        if (millis() - start > timeout_ms) return false;
        delay(1);
    }
    return true;
}

uint16_t SMBus::readWord(uint8_t addr, uint8_t reg) {
    if (!busLock()) { logPush(LOG_READ_WORD, addr, reg, false, -1, nullptr); return 0xFFFF; }
    s_wire->beginTransmission(addr);
    s_wire->write(reg);
    if (s_wire->endTransmission(false) != 0) {
        busUnlock();
        logPush(LOG_READ_WORD, addr, reg, false, -1, nullptr);
        return 0xFFFF;
    }
    s_wire->requestFrom(addr, (uint8_t)2);
    if (!waitBytes(s_wire, 2, 100)) {
        busUnlock();
        logPush(LOG_READ_WORD, addr, reg, false, -1, nullptr);
        return 0xFFFF;
    }
    uint16_t lo = s_wire->read();
    uint16_t hi = s_wire->read();
    busUnlock();
    uint16_t val = (hi << 8) | lo;
    uint8_t d[2] = {(uint8_t)lo, (uint8_t)hi};
    logPush(LOG_READ_WORD, addr, reg, true, 2, d);
    return val;
}

uint32_t SMBus::readDword(uint8_t addr, uint8_t reg) {
    // Primary path: SMBus block read (TI standard for 32-bit status registers).
    uint8_t buf[8] = {0};
    int len = readBlock(addr, reg, buf, 8);
    if (len >= 4) {
        return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
               ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    }
    // Fallback path: PTL 2025 firmware (and possibly other clones) reject
    // block reads on status registers but ACCEPT single-word reads at the
    // same address. The low 16 bits of TI 32-bit status registers contain
    // all the commonly-decoded flags (PRES/DSG/PF/XCHG/SEC bits etc), so
    // returning low_16 | (0 << 16) is enough for our decoders. We use 0
    // (not 0xFFFF) for the high half so caller can distinguish "block read
    // failed -> only got low 16" (high=0) from "fully NACK" (high=0xFFFF).
    // TEST_LOG note #34, observed on Battery #6 PTL 2025-06.
    uint16_t lo = readWord(addr, reg);
    if (lo != 0xFFFF) {
        return (uint32_t)lo;  // high half = 0 -> we know it's the partial-read path
    }
    return 0xFFFFFFFF;
}

int SMBus::readBlock(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t maxLen) {
    if (!busLock()) { logPush(LOG_READ_BLOCK, addr, reg, false, -1, nullptr); return -1; }
    s_wire->beginTransmission(addr);
    s_wire->write(reg);
    if (s_wire->endTransmission(false) != 0) {
        busUnlock();
        logPush(LOG_READ_BLOCK, addr, reg, false, -1, nullptr);
        return -1;
    }
    s_wire->requestFrom(addr, (uint8_t)(maxLen + 1));
    if (!waitBytes(s_wire, 1, 100)) {
        busUnlock();
        logPush(LOG_READ_BLOCK, addr, reg, false, -1, nullptr);
        return -1;
    }
    uint8_t len = s_wire->read();
    if (len > maxLen) len = maxLen;
    if (len > 0) waitBytes(s_wire, len, 100);
    for (int i = 0; i < len && s_wire->available(); i++) {
        buf[i] = s_wire->read();
    }
    busUnlock();
    logPush(LOG_READ_BLOCK, addr, reg, true, len, buf);
    return len;
}

String SMBus::readString(uint8_t addr, uint8_t reg) {
    uint8_t buf[32] = {0};
    int len = readBlock(addr, reg, buf, 31);
    if (len <= 0) return "";
    buf[len] = '\0';
    return String((char*)buf);
}

bool SMBus::writeWord(uint8_t addr, uint8_t reg, uint16_t value) {
    if (!busLock()) { logPush(LOG_WRITE_WORD, addr, reg, false, -1, nullptr); return false; }
    s_wire->beginTransmission(addr);
    s_wire->write(reg);
    s_wire->write(value & 0xFF);
    s_wire->write((value >> 8) & 0xFF);
    bool ok = s_wire->endTransmission() == 0;
    busUnlock();
    uint8_t d[2] = {(uint8_t)(value & 0xFF), (uint8_t)(value >> 8)};
    logPush(LOG_WRITE_WORD, addr, reg, ok, 2, d);
    return ok;
}

// SMBus Packet Error Check: CRC-8 polynomial 0x07, init 0x00.
uint8_t SMBus::smbusPEC(const uint8_t *data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
        }
    }
    return crc;
}

bool SMBus::writeWordPEC(uint8_t addr, uint8_t reg, uint16_t value) {
    if (!busLock()) { logPush(LOG_WRITE_WORD, addr, reg, false, -1, nullptr); return false; }
    uint8_t frame[5] = {(uint8_t)(addr << 1), reg, (uint8_t)(value & 0xFF), (uint8_t)(value >> 8)};
    frame[4] = smbusPEC(frame, 4);  // PEC over write_addr + reg + data
    s_wire->beginTransmission(addr);
    s_wire->write(reg);
    s_wire->write(value & 0xFF);
    s_wire->write((value >> 8) & 0xFF);
    s_wire->write(frame[4]);
    bool ok = s_wire->endTransmission() == 0;
    busUnlock();
    uint8_t d[3] = {(uint8_t)(value & 0xFF), (uint8_t)(value >> 8), frame[4]};
    logPush(LOG_WRITE_WORD, addr, reg, ok, 3, d);
    return ok;
}

bool SMBus::writeBlockPEC(uint8_t addr, uint8_t reg, const uint8_t *data, uint8_t len) {
    if (!busLock()) { logPush(LOG_WRITE_BLOCK, addr, reg, false, -1, nullptr); return false; }
    // Build frame for PEC calculation: write_addr + reg + len + data
    uint8_t frame[36];
    frame[0] = addr << 1;
    frame[1] = reg;
    frame[2] = len;
    for (uint8_t i = 0; i < len; i++) frame[3+i] = data[i];
    uint8_t pec = smbusPEC(frame, 3 + len);
    s_wire->beginTransmission(addr);
    s_wire->write(reg);
    s_wire->write(len);
    for (uint8_t i = 0; i < len; i++) s_wire->write(data[i]);
    s_wire->write(pec);
    bool ok = s_wire->endTransmission() == 0;
    busUnlock();
    logPush(LOG_WRITE_BLOCK, addr, reg, ok, len, data);
    return ok;
}

bool SMBus::writeBlock(uint8_t addr, uint8_t reg, const uint8_t *data, uint8_t len) {
    if (!busLock()) { logPush(LOG_WRITE_BLOCK, addr, reg, false, -1, nullptr); return false; }
    s_wire->beginTransmission(addr);
    s_wire->write(reg);
    s_wire->write(len);
    for (uint8_t i = 0; i < len; i++) s_wire->write(data[i]);
    bool ok = s_wire->endTransmission() == 0;
    busUnlock();
    logPush(LOG_WRITE_BLOCK, addr, reg, ok, len, data);
    return ok;
}

// Write MAC subcommand to register 0x00 (for simple commands)
bool SMBus::macCommand(uint8_t addr, uint16_t subcommand) {
    bool ok = writeWord(addr, 0x00, subcommand);
    // writeWord already logs; override with MAC_CMD type
    if (s_logOn && s_logHead > 0) {
        auto &e = s_log[(s_logHead - 1) % LOG_SIZE];
        e.op = LOG_MAC_CMD;
        e.reg = (uint8_t)(subcommand & 0xFF);  // low byte for display
    }
    return ok;
}

// Write subcommand to MAC 0x44 (ManufacturerBlockAccess) then read block response
int SMBus::macBlockRead(uint8_t addr, uint16_t subcommand, uint8_t *buf, uint8_t maxLen) {
    // Temporarily disable logging for the intermediate write+read
    bool wasLogging = s_logOn;
    s_logOn = false;

    // Write subcommand as 2-byte block to 0x44
    uint8_t cmd[2] = { (uint8_t)(subcommand & 0xFF), (uint8_t)((subcommand >> 8) & 0xFF) };
    if (!writeBlock(addr, 0x44, cmd, 2)) {
        s_logOn = wasLogging;
        logPush(LOG_MAC_BLOCK_READ, addr, (uint8_t)(subcommand & 0xFF), false, -1, nullptr);
        return -1;
    }
    delay(10);

    // Read block from 0x44 — first 2 bytes echo subcommand, then data
    uint8_t tmp[40] = {0};
    int total = readBlock(addr, 0x44, tmp, sizeof(tmp) - 1);
    s_logOn = wasLogging;

    if (total < 2) {
        logPush(LOG_MAC_BLOCK_READ, addr, (uint8_t)(subcommand & 0xFF), false, -1, nullptr);
        return -1;
    }

    // Strip the 2-byte subcommand echo
    int dataLen = total - 2;
    if (dataLen < 0) dataLen = 0;
    if (dataLen > maxLen) dataLen = maxLen;
    for (int i = 0; i < dataLen; i++) buf[i] = tmp[i + 2];
    logPush(LOG_MAC_BLOCK_READ, addr, (uint8_t)(subcommand & 0xFF), true, dataLen, buf);
    return dataLen;
}

// ===== I2C Preflight diagnostics =====
SMBus::PreflightResult SMBus::preflight() {
    PreflightResult r = {};

    // Check line levels (should be HIGH with pull-ups)
    r.sdaOk = digitalRead(s_sda) == HIGH;
    r.sclOk = digitalRead(s_scl) == HIGH;

    // If lines stuck, try recovery
    if (!r.sdaOk || !r.sclOk) {
        i2cRecover();
        delay(10);
        r.sdaOk = digitalRead(s_sda) == HIGH;
        r.sclOk = digitalRead(s_scl) == HIGH;
    }

    // Try bus init
    s_wire->begin(s_sda, s_scl);
    s_wire->setClock(100000);
    s_wire->setTimeOut(50);
    r.busOk = true;  // Wire.begin doesn't return error on ESP32

    // Scan bus
    r.devCount = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        s_wire->beginTransmission(a);
        if (s_wire->endTransmission() == 0) {
            if (a == 0x0B) r.batteryAck = true;
            if (r.devCount < 8) r.devAddrs[r.devCount] = a;
            r.devCount++;
        }
    }

    return r;
}
