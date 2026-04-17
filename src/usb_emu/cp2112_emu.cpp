/* CP2112 HID-to-SMBus Bridge emulator for ESP32-S3 (direct TinyUSB).
 *
 * Emulates Silicon Labs CP2112 device (VID 10C4:EA90) so standard CP2112
 * tools (DJI Battery Killer, bqStudio-lite, user scripts using
 * SLABHIDtoSMBus.dll) work transparently on ESP32 without a shim DLL.
 *
 * Uses TinyUSB HID callbacks directly (NOT Arduino's USBHID wrapper) because
 * that wrapper silently drops Output reports whose IDs aren't in its parsed
 * device->report_ids table — SiLabs DLL sends Output 0x17 (Data Write) via
 * control-transfer SET_REPORT and we must catch it ourselves.
 *
 * Reports (per CP2112 AN495):
 *   Output  0x01  Reset Device
 *   Feature 0x02  GPIO Configuration (4 bytes)
 *   Feature 0x03  GPIO Get (1 byte)
 *   Feature 0x04  GPIO Set (2 bytes)
 *   Feature 0x05  Get Version Info (2 bytes)
 *   Feature 0x06  SMBus config (13 bytes)
 *   Output  0x10  Read Request       slave + len(2 BE)
 *   Output  0x11  Write-Read Request slave + len(2) + tgtLen + tgt
 *   Output  0x12  Transfer Status Request
 *   Output  0x13  Force Read Response
 *   Input   0x14  Read Response      status + len + data[61]
 *   Input   0x15  Transfer Status    6 bytes
 *   Output  0x16  Cancel Transfer
 *   Output  0x17  Data Write         slave + len + data
 *
 * I2C backend: Wire1 on BATT_SDA/BATT_SCL (DJIBattery::init owns the bus).
 */

#include "cp2112_emu.h"
#include "pin_config.h"
#include <Wire.h>
#include "USB.h"
#include "esp32-hal-tinyusb.h"
#include "battery/smbus.h"  // for I2C bus mutex
#include "class/hid/hid.h"
#include "class/hid/hid_device.h"
#include "tusb.h"

namespace {

// ============ CP2112 HID Report Descriptor (canonical per AN495 / Linux) ============
// Report ID directions MUST match what SLABHIDtoSMBus.dll expects — the DLL
// parses this via HidP_GetCaps and routes WriteFile/ReadFile accordingly.
//   OUT (host→dev): 0x10 0x11 0x12 0x14 0x15 0x17
//   IN  (dev→host): 0x13 0x16
//   FEATURE:        0x02 0x03 0x04 0x05 0x06 0x20..0x24
const uint8_t hid_report_desc[] = {
    0x06, 0x00, 0xFF,       // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,             // Usage (0x01)
    0xA1, 0x01,             // Collection (Application)

    0x15, 0x00,             //   Logical Minimum (0)
    0x26, 0xFF, 0x00,       //   Logical Maximum (255)
    0x75, 0x08,             //   Report Size (8 bits)

    // --- Output reports (host → device) ---
    0x85, 0x10, 0x95, 0x03, 0x09, 0x01, 0x91, 0x02,   // 0x10 Read Request (3B): slave + len[2 BE]
    0x85, 0x11, 0x95, 0x14, 0x09, 0x01, 0x91, 0x02,   // 0x11 Write-Read Req (20B): slave + len[2] + tgtLen + tgt[16]
    0x85, 0x12, 0x95, 0x02, 0x09, 0x01, 0x91, 0x02,   // 0x12 Data Read Force Send (2B): len[2 BE]
    0x85, 0x14, 0x95, 0x3F, 0x09, 0x01, 0x91, 0x02,   // 0x14 Data Write (63B): slave + len + data[61]
    0x85, 0x15, 0x95, 0x01, 0x09, 0x01, 0x91, 0x02,   // 0x15 Transfer Status Req (1B dummy)
    0x85, 0x17, 0x95, 0x01, 0x09, 0x01, 0x91, 0x02,   // 0x17 Cancel Transfer (1B dummy)

    // --- Input reports (device → host) ---
    0x85, 0x13, 0x95, 0x3F, 0x09, 0x01, 0x81, 0x02,   // 0x13 Read Response (63B): status + len + data[61]
    0x85, 0x16, 0x95, 0x06, 0x09, 0x01, 0x81, 0x02,   // 0x16 Transfer Status Resp (6B): s0 + s1 + retries[2] + bytesRead[2]

    // --- Feature reports ---
    0x85, 0x02, 0x95, 0x04, 0x09, 0x01, 0xB1, 0x02,   // 0x02 GPIO Configuration (4B)
    0x85, 0x03, 0x95, 0x01, 0x09, 0x01, 0xB1, 0x02,   // 0x03 GPIO Get (1B)
    0x85, 0x04, 0x95, 0x02, 0x09, 0x01, 0xB1, 0x02,   // 0x04 GPIO Set (2B)
    0x85, 0x05, 0x95, 0x02, 0x09, 0x01, 0xB1, 0x02,   // 0x05 Get Version (2B)
    0x85, 0x06, 0x95, 0x0D, 0x09, 0x01, 0xB1, 0x02,   // 0x06 SMBus Configuration (13B)
    0x85, 0x20, 0x95, 0x01, 0x09, 0x01, 0xB1, 0x02,   // 0x20 Lock Byte (1B)
    0x85, 0x21, 0x95, 0x09, 0x09, 0x01, 0xB1, 0x02,   // 0x21 USB Config (9B): VID+PID+power+powerMode+releaseVer+mask
    0x85, 0x22, 0x95, 0x3E, 0x09, 0x01, 0xB1, 0x02,   // 0x22 Manufacturer String (62B)
    0x85, 0x23, 0x95, 0x3E, 0x09, 0x01, 0xB1, 0x02,   // 0x23 Product String (62B)
    0x85, 0x24, 0x95, 0x3E, 0x09, 0x01, 0xB1, 0x02,   // 0x24 Serial String (62B)

    0xC0,                   // End Collection
};

struct SmbusConfig {
    uint32_t bitRate      = 100000;
    uint8_t  address      = 0x02;
    uint8_t  autoRead     = 0;
    uint16_t writeTimeout = 200;
    uint16_t readTimeout  = 200;
    uint8_t  sclLow       = 0;
    uint16_t retries      = 0;
};

SmbusConfig g_cfg;

uint8_t  g_gpio_cfg[4] = { 0, 0, 0, 0 };
uint8_t  g_gpio_val    = 0xFF;

uint8_t  g_readBuf[62];
uint16_t g_readLen    = 0;
uint8_t  g_readStatus = 0;     // 0=idle 1=busy 2=complete 3=error
uint8_t  g_status0    = 0;     // Transfer status
uint8_t  g_status1    = 0;
uint16_t g_retries    = 0;
uint16_t g_bytesRead  = 0;

volatile bool g_active = false;

// ============ Transaction log (ring buffer for web diagnostics) ============
struct LogEntry {
    uint32_t ms;
    char     dir;       // 'O'utput 'G'etFeature 'F'=SetFeature 'S'end-input 'W'/'R'/'X' I2C ops
    uint8_t  slave7;
    uint8_t  err;
    uint8_t  reqLen;
    uint8_t  gotLen;
    uint8_t  data[16];
};
static const int LOG_CAP = 64;
static LogEntry  g_log[LOG_CAP];
static volatile uint16_t g_logHead = 0;
static volatile uint32_t g_logSeq  = 0;

static void logAdd(char dir, uint8_t slave7, uint8_t err,
                   uint8_t reqLen, uint8_t gotLen, const uint8_t *data) {
    LogEntry &e = g_log[g_logHead];
    e.ms     = millis();
    e.dir    = dir;
    e.slave7 = slave7;
    e.err    = err;
    e.reqLen = reqLen;
    e.gotLen = gotLen;
    uint8_t n = gotLen > 16 ? 16 : gotLen;
    if (data && n) memcpy(e.data, data, n); else memset(e.data, 0, 16);
    g_logHead = (g_logHead + 1) % LOG_CAP;
    g_logSeq++;
}

} // namespace

// Exposed to web_server.cpp
extern "C" uint32_t cp2112_log_seq() { return g_logSeq; }
extern "C" int cp2112_log_dump(char *out, int cap) {
    int n = 0;
    uint16_t idx = g_logHead;
    for (int i = 0; i < LOG_CAP && n < cap - 128; i++) {
        idx = (idx + LOG_CAP - 1) % LOG_CAP;
        const LogEntry &e = g_log[idx];
        if (e.ms == 0) break;
        n += snprintf(out + n, cap - n,
                      "%lu %c s=0x%02X err=%u r=%u g=%u ",
                      (unsigned long)e.ms, e.dir, e.slave7, e.err, e.reqLen, e.gotLen);
        uint8_t show = e.gotLen > 16 ? 16 : e.gotLen;
        for (uint8_t b = 0; b < show && n < cap - 4; b++) {
            n += snprintf(out + n, cap - n, "%02X ", e.data[b]);
        }
        if (n < cap - 2) out[n++] = '\n';
    }
    if (n < cap) out[n] = 0;
    return n;
}

// ============ I2C backend ============

namespace {

bool doWrite(uint8_t slave7, const uint8_t *data, uint8_t len) {
    if (!SMBus::busLock(500)) { g_status0 = 3; g_status1 = 4; return false; }
    Wire1.beginTransmission(slave7);
    Wire1.write(data, len);
    uint8_t err = Wire1.endTransmission();
    SMBus::busUnlock();
    g_status0 = (err == 0) ? 2 : 3;
    g_status1 = err;
    logAdd('W', slave7, err, len, len, data);
    return err == 0;
}

void emitReadResponse();

bool doWriteRead(uint8_t slave7, const uint8_t *tgt, uint8_t tgtLen, uint16_t rLen) {
    if (!SMBus::busLock(500)) { g_status0 = 3; g_status1 = 4; g_readLen = 0; return false; }
    Wire1.beginTransmission(slave7);
    Wire1.write(tgt, tgtLen);
    uint8_t err = Wire1.endTransmission(false);
    if (err != 0) {
        Wire1.beginTransmission(slave7);
        Wire1.write(tgt, tgtLen);
        err = Wire1.endTransmission(true);
        if (err != 0) {
            SMBus::busUnlock();
            g_status0 = 3; g_status1 = err; g_readLen = 0;
            logAdd('X', slave7, err, tgtLen, 0, tgt);
            if (g_cfg.autoRead) emitReadResponse();
            return false;
        }
    }
    if (rLen > sizeof(g_readBuf)) rLen = sizeof(g_readBuf);
    Wire1.requestFrom(slave7, (uint8_t)rLen);
    uint16_t got = 0;
    uint32_t t0 = millis();
    while (got < rLen && (millis() - t0) < g_cfg.readTimeout) {
        if (Wire1.available()) g_readBuf[got++] = Wire1.read();
    }
    SMBus::busUnlock();
    g_readLen = got; g_readStatus = 2; g_status0 = 2; g_bytesRead = got;
    logAdd('X', slave7, 0, tgtLen, (uint8_t)got, g_readBuf);
    if (g_cfg.autoRead) emitReadResponse();
    return true;
}

bool doRead(uint8_t slave7, uint16_t rLen) {
    if (!SMBus::busLock(500)) { g_status0 = 3; g_status1 = 4; g_readLen = 0; return false; }
    if (rLen > sizeof(g_readBuf)) rLen = sizeof(g_readBuf);
    Wire1.requestFrom(slave7, (uint8_t)rLen);
    uint16_t got = 0;
    uint32_t t0 = millis();
    while (got < rLen && (millis() - t0) < g_cfg.readTimeout) {
        if (Wire1.available()) g_readBuf[got++] = Wire1.read();
    }
    SMBus::busUnlock();
    g_readLen = got; g_readStatus = 2; g_status0 = 2; g_bytesRead = got;
    logAdd('R', slave7, 0, (uint8_t)rLen, (uint8_t)got, g_readBuf);
    if (g_cfg.autoRead) emitReadResponse();
    return true;
}

// HID instance is always 0 here (single HID interface in our composite)
static uint8_t g_hid_instance = 0;

// Send Input Report 0x13 Data Read Response (fixed 63-byte payload per AN495)
void emitReadResponse() {
    uint8_t p[63] = {0};
    p[0] = g_readStatus;
    uint8_t copy = g_readLen > 61 ? 61 : (uint8_t)g_readLen;
    p[1] = copy;
    if (copy) memcpy(p + 2, g_readBuf, copy);
    bool ok = tud_hid_n_report(g_hid_instance, 0x13, p, 63);
    logAdd('S', 0x13, ok ? 0 : 0xFF, copy, 16, p);
}

// Send Input Report 0x16 Transfer Status Response (6 bytes)
void emitStatusResponse() {
    uint8_t p[6] = {
        g_status0, g_status1,
        (uint8_t)(g_retries >> 8),   (uint8_t)(g_retries & 0xFF),
        (uint8_t)(g_bytesRead >> 8), (uint8_t)(g_bytesRead & 0xFF)
    };
    bool ok = tud_hid_n_report(g_hid_instance, 0x16, p, 6);
    logAdd('S', 0x16, ok ? 0 : 0xFF, 6, 6, p);
}

// ============ Interface descriptor loader ============
static uint8_t g_ep_in = 0, g_ep_out = 0, g_itf_num = 0;

uint16_t load_hid_descriptor(uint8_t *dst, uint8_t *itf) {
    uint8_t str_index = tinyusb_add_string_descriptor("CP2112 HID");
    g_ep_in  = tinyusb_get_free_in_endpoint();
    g_ep_out = tinyusb_get_free_out_endpoint();
    g_itf_num = *itf;
    uint8_t descriptor[TUD_HID_INOUT_DESC_LEN] = {
        TUD_HID_INOUT_DESCRIPTOR(
            *itf, str_index, HID_ITF_PROTOCOL_NONE, sizeof(hid_report_desc),
            g_ep_out, (uint8_t)(0x80 | g_ep_in), 64, 1
        )
    };
    *itf += 1;
    memcpy(dst, descriptor, TUD_HID_INOUT_DESC_LEN);
    return TUD_HID_INOUT_DESC_LEN;
}

extern "C" int cp2112_ep_info(char *out, int cap) {
    return snprintf(out, cap, "itf=%u ep_in=0x%02X ep_out=0x%02X desc_len=%u\n",
                    g_itf_num, g_ep_in, g_ep_out, (unsigned)sizeof(hid_report_desc));
}

// ============ Feature / Output report handlers ============

uint16_t handleGetFeature(uint8_t report_id, uint8_t *buffer, uint16_t reqlen) {
    logAdd('G', report_id, 0, (uint8_t)(reqlen > 255 ? 255 : reqlen), 0, nullptr);

    // Zero the whole requested buffer; some HID drivers (SiLabs SLABHIDtoSMBus)
    // send wLength matching the max report size and interpret a short transfer
    // as failure. Fill with zeros, overwrite valid bytes, return reqlen.
    memset(buffer, 0, reqlen);

    switch (report_id) {
    case 0x02:
        if (reqlen < 4) return 0;
        memcpy(buffer, g_gpio_cfg, 4);
        return reqlen;
    case 0x03:
        if (reqlen < 1) return 0;
        buffer[0] = g_gpio_val;
        return reqlen;
    case 0x04:
        if (reqlen < 2) return 0;
        buffer[0] = g_gpio_val; buffer[1] = 0xFF;
        return reqlen;
    case 0x05: // Get Version: partNum=0x0C (CP2112), version=0x01
        if (reqlen < 2) return 0;
        buffer[0] = 0x0C; buffer[1] = 0x01;
        return reqlen;
    case 0x06: // SMBus Config (13 bytes, big-endian)
        if (reqlen < 13) return 0;
        buffer[0]  = (g_cfg.bitRate >> 24) & 0xFF;
        buffer[1]  = (g_cfg.bitRate >> 16) & 0xFF;
        buffer[2]  = (g_cfg.bitRate >> 8)  & 0xFF;
        buffer[3]  = g_cfg.bitRate & 0xFF;
        buffer[4]  = g_cfg.address;
        buffer[5]  = g_cfg.autoRead;
        buffer[6]  = (g_cfg.writeTimeout >> 8) & 0xFF;
        buffer[7]  = g_cfg.writeTimeout & 0xFF;
        buffer[8]  = (g_cfg.readTimeout >> 8) & 0xFF;
        buffer[9]  = g_cfg.readTimeout & 0xFF;
        buffer[10] = g_cfg.sclLow;
        buffer[11] = (g_cfg.retries >> 8) & 0xFF;
        buffer[12] = g_cfg.retries & 0xFF;
        return reqlen;
    case 0x13: // Input Read Response via HidD_GetInputReport
        if (reqlen < 63) return 0;
        buffer[0] = g_readStatus;
        {
            uint8_t copy = g_readLen > 61 ? 61 : (uint8_t)g_readLen;
            buffer[1] = copy;
            if (copy) memcpy(buffer + 2, g_readBuf, copy);
        }
        return reqlen;
    case 0x16: // Input Transfer Status via HidD_GetInputReport
        if (reqlen < 6) return 0;
        buffer[0] = g_status0;
        buffer[1] = g_status1;
        buffer[2] = (g_retries >> 8) & 0xFF;
        buffer[3] = g_retries & 0xFF;
        buffer[4] = (g_bytesRead >> 8) & 0xFF;
        buffer[5] = g_bytesRead & 0xFF;
        return reqlen;
    case 0x20: // Lock Byte — 0xFF = unlocked (all customization allowed)
        if (reqlen < 1) return 0;
        buffer[0] = 0xFF;
        return reqlen;
    case 0x21: // USB Configuration (9 bytes): VID(2 LE), PID(2 LE), MaxPower(1), PowerMode(1), ReleaseVer(2 LE), Mask(1)
        if (reqlen < 9) return 0;
        buffer[0] = 0xC4; buffer[1] = 0x10;   // VID 0x10C4 (LE)
        buffer[2] = 0x90; buffer[3] = 0xEA;   // PID 0xEA90 (LE)
        buffer[4] = 0xFA;                     // MaxPower 0xFA*2 = 500mA
        buffer[5] = 0x00;                     // PowerMode: bus-powered regulator on
        buffer[6] = 0x00; buffer[7] = 0x01;   // Release 1.00 (LE)
        buffer[8] = 0x00;                     // Mask: nothing customised
        return reqlen;
    case 0x22: // Manufacturer String
    case 0x23: // Product String
    case 0x24: // Serial String
        if (reqlen < 2) return 0;
        // Bytes: [length, type=0x03 UTF-16LE, data...]
        buffer[0] = 2;          // minimal: just length byte + type
        buffer[1] = 0x03;       // string descriptor type
        return reqlen;
    default:
        return 0;
    }
}

void handleOutput(uint8_t report_id, const uint8_t *buffer, uint16_t len) {
    uint8_t logBuf[16];
    uint8_t n = len > 16 ? 16 : (uint8_t)len;
    memcpy(logBuf, buffer, n);
    logAdd('O', report_id, 0, (uint8_t)(len > 255 ? 255 : len), n, logBuf);

    switch (report_id) {
    case 0x10: { // Data Read Request: slave8 + len(2 BE)
        if (len < 3) return;
        uint8_t  slave7 = buffer[0] >> 1;
        uint16_t rLen   = ((uint16_t)buffer[1] << 8) | buffer[2];
        g_status0 = 1;
        doRead(slave7, rLen);
        break;
    }
    case 0x11: { // Data Write-Read Request: slave8 + len(2) + tgtLen + tgt[16]
        if (len < 5) return;
        uint8_t  slave7 = buffer[0] >> 1;
        uint16_t rLen   = ((uint16_t)buffer[1] << 8) | buffer[2];
        uint8_t  tgtLen = buffer[3];
        if (tgtLen > 16) tgtLen = 16;
        if (len < 4U + tgtLen) return;
        g_status0 = 1;
        doWriteRead(slave7, buffer + 4, tgtLen, rLen);
        break;
    }
    case 0x12: // Data Read Force Send: len[2 BE] — emit Input 0x13
        emitReadResponse();
        break;
    case 0x14: { // Data Write Request: slave8 + len + data[]
        if (len < 2) return;
        uint8_t slave7 = buffer[0] >> 1;
        uint8_t wLen   = buffer[1];
        if (len < 2U + wLen) return;
        g_status0 = 1;
        doWrite(slave7, buffer + 2, wLen);
        break;
    }
    case 0x15: // Transfer Status Request — emit Input 0x16
        emitStatusResponse();
        break;
    case 0x17: // Cancel Transfer
        g_status0 = 0; g_readLen = 0;
        break;
    default:
        break;
    }
}

void handleSetFeature(uint8_t report_id, const uint8_t *buffer, uint16_t len) {
    uint8_t logBuf[16];
    uint8_t n = len > 16 ? 16 : (uint8_t)len;
    memcpy(logBuf, buffer, n);
    logAdd('F', report_id, 0, (uint8_t)(len > 255 ? 255 : len), n, logBuf);

    switch (report_id) {
    case 0x02: // GPIO Configuration
        if (len >= 4) memcpy(g_gpio_cfg, buffer, 4);
        break;
    case 0x04: // GPIO Set: value, mask
        if (len >= 2) {
            uint8_t mask = buffer[1];
            g_gpio_val = (g_gpio_val & ~mask) | (buffer[0] & mask);
        }
        break;
    case 0x06: // SMBus Configuration
        if (len >= 13) {
            g_cfg.bitRate = ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16)
                          | ((uint32_t)buffer[2] << 8)  | (uint32_t)buffer[3];
            g_cfg.address = buffer[4];
            g_cfg.autoRead = buffer[5];
            g_cfg.writeTimeout = ((uint16_t)buffer[6] << 8) | buffer[7];
            g_cfg.readTimeout  = ((uint16_t)buffer[8] << 8) | buffer[9];
            g_cfg.sclLow = buffer[10];
            g_cfg.retries = ((uint16_t)buffer[11] << 8) | buffer[12];
            Wire1.setClock(g_cfg.bitRate);
        }
        break;
    default:
        break;
    }
}

} // namespace

// ============ TinyUSB callbacks (override weak defaults) ============
extern "C" {

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_type;
    return handleGetFeature(report_id, buffer, reqlen);
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize) {
    (void)instance;
    if (report_id == 0) {
        // Came from OUT interrupt endpoint — report_id is first byte of buffer
        if (bufsize == 0) return;
        report_id = buffer[0];
        buffer++;
        bufsize--;
    }
    if (report_type == HID_REPORT_TYPE_FEATURE) {
        handleSetFeature(report_id, buffer, bufsize);
    } else {
        // OUTPUT (or 0=implicit OUT endpoint) — route to Output handler
        handleOutput(report_id, buffer, bufsize);
    }
}

} // extern "C"

// ============ Public API ============

void CP2112_attach() {
    // DJIBattery::init() has already brought Wire1 up on BATT_SDA/BATT_SCL
    // with INPUT_PULLUP. Just update clock speed.
    Wire1.setClock(g_cfg.bitRate);

    // Override ESP32-S3 variant's hardcoded VID/PID before USB.begin() fires.
    USB.VID(0x10C4);
    USB.PID(0xEA90);
    USB.productName("CP2112 HID USB-to-SMBus Bridge");
    USB.manufacturerName("Silicon Labs");
    USB.firmwareVersion(0x0100);      // bcdDevice 1.00 — matches typical CP2112
    // Real CP2112 declares bDeviceClass=0 (defined per interface), not the
    // Arduino default TUSB_CLASS_MISC (IAD). SiLabs driver matches on this.
    USB.usbClass(0x00);
    USB.usbSubClass(0x00);
    USB.usbProtocol(0x00);
    USB.usbAttributes(0x80);          // Bus-powered, not self-powered

    // Register HID interface with TinyUSB directly — bypasses Arduino USBHID
    // class whose report-id parser drops our Output reports.
    tinyusb_enable_interface2(USB_INTERFACE_HID, TUD_HID_INOUT_DESC_LEN,
                              load_hid_descriptor, false);

    g_active = true;
    Serial.println("[CP2112] HID emulator attached (VID 10C4 PID EA90)");
}

void CP2112_loop() {}
bool CP2112_isActive() { return g_active; }
