#include "blheli_4way.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include "core/pin_port.h"

// BLHeli 4way-interface protocol constants
static const uint8_t FRAME_START_HOST = 0x2F;  // PC → FC
static const uint8_t FRAME_START_ESC  = 0x2E;  // FC → PC
static const uint8_t ACK_OK           = 0x00;
static const uint8_t ACK_UNKNOWN_CMD  = 0x02;
static const uint8_t ACK_COMMAND_ERR  = 0x03;
static const uint8_t ACK_CRC_ERR      = 0x04;
static const uint8_t ACK_VERIFY_ERR   = 0x05;

// 4way-interface commands (from BLHeliSuite source / Betaflight serial_4way.c)
enum {
    cmd_InterfaceTestAlive   = 0x30,
    cmd_ProtocolGetVersion   = 0x31,
    cmd_InterfaceGetName     = 0x32,
    cmd_InterfaceGetVersion  = 0x33,
    cmd_InterfaceExit        = 0x34,
    cmd_DeviceReset          = 0x35,
    cmd_DeviceInitFlash      = 0x37,
    cmd_DeviceEraseAll       = 0x38,
    cmd_DevicePageErase      = 0x39,
    cmd_DeviceRead           = 0x3A,
    cmd_DeviceWrite          = 0x3B,
    cmd_DeviceC2CK_LOW       = 0x3C,
    cmd_DeviceReadEEprom     = 0x3D,
    cmd_DeviceWriteEEprom    = 0x3E,
    cmd_InterfaceSetMode     = 0x3F,
};

// 4way interface modes
enum {
    imSK        = 0,  // unused (C2)
    imATM       = 1,  // Atmel ATmega (older)
    imSIL_BLB   = 2,  // Silabs BLHeli Bootloader
    imARM_BLB   = 3,  // ARM BLHeli Bootloader (BLHeli_32)
    imATM_BLB   = 4,  // Atmega BLHeli Bootloader
};

static BLHeli4Way::Status s_status = {};
static AsyncServer       *s_server = nullptr;
static AsyncClient       *s_client = nullptr;
static int                s_signalPin = 2;
static bool               s_running   = false;
static uint8_t            s_currentMode = imARM_BLB;
static uint8_t            s_currentEsc  = 0;

// ---- OneWire bit-bang helpers ----
// BLHeli OneWire = half-duplex serial on signal wire at 19200 baud.
// Signal wire is pulled HIGH by ESC's BEC when idle.
// WRITE: drive signalPin LOW for start bit, then data bits LSB-first, then HIGH stop bit.
// READ: release pin to INPUT, sample at baud timing.
static const uint32_t BIT_PERIOD_US = 1000000 / 19200;  // ~52 us

static void IRAM_ATTR owDrive(int pin, int level) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, level);
}

static void IRAM_ATTR owRelease(int pin) {
    pinMode(pin, INPUT_PULLUP);
}

static void owSendByte(int pin, uint8_t b) {
    // Start bit (LOW)
    owDrive(pin, LOW);
    delayMicroseconds(BIT_PERIOD_US);
    // 8 data bits, LSB first
    for (int i = 0; i < 8; i++) {
        digitalWrite(pin, (b & 1) ? HIGH : LOW);
        b >>= 1;
        delayMicroseconds(BIT_PERIOD_US);
    }
    // Stop bit (HIGH)
    digitalWrite(pin, HIGH);
    delayMicroseconds(BIT_PERIOD_US);
    owRelease(pin);
}

static int owReadByte(int pin, uint32_t timeoutUs = 10000) {
    owRelease(pin);
    // Wait for start bit (HIGH -> LOW)
    uint32_t t0 = micros();
    while (digitalRead(pin) == HIGH) {
        if ((micros() - t0) > timeoutUs) return -1;
    }
    // Sample at middle of start bit
    delayMicroseconds(BIT_PERIOD_US / 2);
    if (digitalRead(pin) != LOW) return -1;  // bad start
    // Sample 8 data bits at bit centers
    uint8_t val = 0;
    for (int i = 0; i < 8; i++) {
        delayMicroseconds(BIT_PERIOD_US);
        if (digitalRead(pin) == HIGH) val |= (1 << i);
    }
    // Wait for stop bit
    delayMicroseconds(BIT_PERIOD_US);
    return val;
}

// ---- BLHeli bootloader protocol (via onewire) ----
// After ESC entered bootloader mode, respond to commands:
//   Cmd: SetAddress, SetBuffer, Flash, Read, Page, Run, RestartBL, ...
// Wrap in ACK/NAK framing.
//
// For minimal 4way implementation: just support DeviceRead (flash memory dump).

static bool blSendCmd(uint8_t cmd, uint16_t addr, const uint8_t *params, uint8_t paramLen) {
    // Build and send BLHeli bootloader frame
    owSendByte(s_signalPin, cmd);
    owSendByte(s_signalPin, addr >> 8);
    owSendByte(s_signalPin, addr & 0xFF);
    owSendByte(s_signalPin, paramLen);
    for (int i = 0; i < paramLen; i++) owSendByte(s_signalPin, params[i]);
    // Wait for ACK
    int ack = owReadByte(s_signalPin, 100000);
    return ack == 0x00;
}

// ---- 4way protocol handlers ----

static uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t crc = 0x0000) {
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }
    return crc;
}

static void sendResponse(AsyncClient *c, uint8_t cmd, uint16_t addr,
                          const uint8_t *data, uint16_t dataLen, uint8_t ack) {
    // Response frame: [0x2E, cmd, addr_hi, addr_lo, len, data..., ack, crc_hi, crc_lo]
    // Note: len = 0 is encoded as 0x00 but means 256 bytes (0x100) for reads.
    uint8_t buf[512];
    int pos = 0;
    buf[pos++] = FRAME_START_ESC;
    buf[pos++] = cmd;
    buf[pos++] = (uint8_t)(addr >> 8);
    buf[pos++] = (uint8_t)(addr & 0xFF);
    buf[pos++] = (uint8_t)(dataLen == 256 ? 0 : dataLen);
    for (int i = 0; i < dataLen && pos < (int)sizeof(buf) - 3; i++) buf[pos++] = data[i];
    buf[pos++] = ack;
    uint16_t crc = crc16_ccitt(buf, pos);
    buf[pos++] = (uint8_t)(crc >> 8);
    buf[pos++] = (uint8_t)(crc & 0xFF);
    c->write((const char *)buf, pos, ASYNC_WRITE_FLAG_COPY);
}

// Accumulator state for parsing incoming frames (multi-packet support)
struct Accum {
    uint8_t  buf[520];
    int      pos;
};
static Accum s_acc = {};

static void onData(void *arg, AsyncClient *c, void *data, size_t len) {
    const uint8_t *src = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        if (s_acc.pos == 0 && src[i] != FRAME_START_HOST) continue;
        if (s_acc.pos < (int)sizeof(s_acc.buf)) s_acc.buf[s_acc.pos++] = src[i];
        // Minimum 8 bytes for valid frame: start + cmd + addr(2) + len + ack-slot(? no) + crc(2)
        // Wait until we have full frame: header (5 bytes) + param_len + 2 CRC bytes
        if (s_acc.pos >= 7) {
            uint8_t plen = s_acc.buf[4];
            int expected = 5 + (plen == 0 ? 256 : plen) + 2;
            if (s_acc.pos >= expected) {
                // Verify CRC
                uint16_t got = ((uint16_t)s_acc.buf[expected-2] << 8) | s_acc.buf[expected-1];
                uint16_t want = crc16_ccitt(s_acc.buf, expected-2);
                s_status.commandsHandled++;
                if (got != want) {
                    sendResponse(c, s_acc.buf[1], 0, nullptr, 0, ACK_CRC_ERR);
                } else {
                    // Dispatch command
                    uint8_t  cmd   = s_acc.buf[1];
                    uint16_t addr  = ((uint16_t)s_acc.buf[2] << 8) | s_acc.buf[3];
                    const uint8_t *params = &s_acc.buf[5];
                    uint8_t  plenReal = plen == 0 ? 0 : plen;  // actual param count

                    switch (cmd) {
                        case cmd_InterfaceTestAlive:
                            s_status.lastCmdName = "TestAlive";
                            sendResponse(c, cmd, addr, nullptr, 0, ACK_OK);
                            break;
                        case cmd_ProtocolGetVersion: {
                            s_status.lastCmdName = "GetVersion";
                            uint8_t ver = 107;  // 4way protocol version 107
                            sendResponse(c, cmd, addr, &ver, 1, ACK_OK);
                            break;
                        }
                        case cmd_InterfaceGetName: {
                            s_status.lastCmdName = "GetName";
                            const char *name = "FPVMUL4W";  // 8 chars
                            sendResponse(c, cmd, addr, (const uint8_t *)name, 8, ACK_OK);
                            break;
                        }
                        case cmd_InterfaceGetVersion: {
                            s_status.lastCmdName = "GetVersion";
                            uint8_t v[2] = {2, 0};  // interface v2.0
                            sendResponse(c, cmd, addr, v, 2, ACK_OK);
                            break;
                        }
                        case cmd_InterfaceExit:
                            s_status.lastCmdName = "Exit";
                            sendResponse(c, cmd, addr, nullptr, 0, ACK_OK);
                            c->close(true);
                            break;
                        case cmd_InterfaceSetMode:
                            s_status.lastCmdName = "SetMode";
                            if (plenReal >= 1) s_currentMode = params[0];
                            sendResponse(c, cmd, addr, nullptr, 0, ACK_OK);
                            break;
                        case cmd_DeviceInitFlash: {
                            s_status.lastCmdName = "DeviceInitFlash";
                            // Return ESC signature (4 bytes). Real impl would talk to ESC
                            // via onewire; here we return saved signature or stub.
                            uint8_t sig[4] = {0xFF, 0xFF, 0xFF, 0xFF};  // unknown
                            if (s_status.escSignature[0] != 0) memcpy(sig, s_status.escSignature, 4);
                            sendResponse(c, cmd, addr, sig, 4, ACK_OK);
                            break;
                        }
                        case cmd_DeviceRead: {
                            s_status.lastCmdName = "DeviceRead";
                            // Minimal stub: return all 0xFF (would read from ESC flash).
                            // Proper implementation: send BLHeli "R" command to ESC via onewire.
                            uint8_t n = plenReal == 0 ? 0 : params[0];
                            if (n == 0) n = 0;  // spec: 0 means 256
                            uint16_t readLen = n == 0 ? 256 : n;
                            static uint8_t readBuf[256];
                            memset(readBuf, 0xFF, readLen);
                            s_status.escReadBytes += readLen;
                            sendResponse(c, cmd, addr, readBuf, readLen, ACK_OK);
                            break;
                        }
                        case cmd_DeviceWrite:
                            s_status.lastCmdName = "DeviceWrite";
                            // Stub: accept without actual write
                            s_status.escWriteBytes += plenReal;
                            sendResponse(c, cmd, addr, nullptr, 0, ACK_OK);
                            break;
                        default:
                            s_status.lastCmdName = "UnknownCmd";
                            sendResponse(c, cmd, addr, nullptr, 0, ACK_UNKNOWN_CMD);
                            break;
                    }
                }
                // Shift remaining bytes left
                int remaining = s_acc.pos - expected;
                if (remaining > 0) memmove(s_acc.buf, s_acc.buf + expected, remaining);
                s_acc.pos = remaining;
            }
        }
    }
}

static void onConnect(void *arg, AsyncClient *c) {
    s_client = c;
    s_status.clientConnected = true;
    s_acc.pos = 0;
    c->onData(&onData);
    c->onDisconnect([](void *, AsyncClient *cc) {
        s_status.clientConnected = false;
        s_client = nullptr;
    });
}

void BLHeli4Way::start(int signalPin) {
    if (s_running) return;
    if (!PinPort::acquire(PinPort::PORT_B, PORT_GPIO, "blheli_4way")) {
        Serial.println("[BLHeli4Way] Port B busy — switch to GPIO in System → Port B Mode");
        return;
    }
    s_signalPin = signalPin;
    s_server = new AsyncServer(4321);
    s_server->onClient([](void *, AsyncClient *c) { onConnect(nullptr, c); }, nullptr);
    s_server->begin();
    s_running = true;
    Serial.printf("[BLHeli4Way] TCP server on port 4321, ESC pin GPIO %d\n", signalPin);
}

void BLHeli4Way::stop() {
    if (!s_running) return;
    if (s_client) s_client->close(true);
    if (s_server) { s_server->end(); delete s_server; s_server = nullptr; }
    s_running = false;
    PinPort::release(PinPort::PORT_B);
}

bool BLHeli4Way::isRunning() { return s_running; }
const BLHeli4Way::Status &BLHeli4Way::status() { return s_status; }
