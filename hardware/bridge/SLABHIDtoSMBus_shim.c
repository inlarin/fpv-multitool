/* Fake SLABHIDtoSMBus.dll — logging shim for DJI Battery Killer.
 *
 * All 42 exports stubbed. Critical ones log calls to C:\Temp\smbus_log.txt.
 * Returns success for open/config so Battery Killer thinks adapter is ready.
 *
 * For WriteRequest/ReadRequest/AddressReadRequest: log the bytes.
 * GetReadResponse returns last fake data (configurable).
 *
 * Build: gcc -shared -o SLABHIDtoSMBus.dll SLABHIDtoSMBus_shim.c -Wl,--kill-at
 * Place alongside DJIBatteryKiller.exe (renames original first).
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LOG_PATH  "C:\\Temp\\smbus_log.txt"
#define COM_PORT  "\\\\.\\COM5"   /* ESP32 USB CDC — change if different */

static HANDLE log_mutex = NULL;
static HANDLE g_com = INVALID_HANDLE_VALUE;
static HANDLE g_com_mutex = NULL;
static BOOL   g_bridge_active = FALSE;

/* ============= Serial bridge to ESP32 ============= */

static uint8_t crc8_bridge(const uint8_t *b, int n) {
    uint8_t c = 0;
    for (int i = 0; i < n; i++) {
        c ^= b[i];
        for (int j = 0; j < 8; j++) c = (c & 0x80) ? (c << 1) ^ 0x07 : (c << 1);
    }
    return c;
}

static BOOL com_open(void) {
    if (g_com != INVALID_HANDLE_VALUE) return TRUE;
    g_com = CreateFileA(COM_PORT, GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_com == INVALID_HANDLE_VALUE) return FALSE;
    DCB dcb = { sizeof(dcb) };
    GetCommState(g_com, &dcb);
    dcb.BaudRate = 115200;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    SetCommState(g_com, &dcb);
    COMMTIMEOUTS to = {0};
    to.ReadIntervalTimeout = 10;
    to.ReadTotalTimeoutConstant = 200;
    to.WriteTotalTimeoutConstant = 200;
    SetCommTimeouts(g_com, &to);
    PurgeComm(g_com, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return TRUE;
}

/* Send a framed command, receive response. Returns bytes received (or -1).
 * Response placed in resp[]. resp_status written on success. */
static int bridge_cmd(uint8_t cmd, const uint8_t *args, uint8_t argLen,
                      uint8_t *resp_status, uint8_t *resp, int resp_max) {
    if (!com_open()) return -1;
    if (!g_com_mutex) g_com_mutex = CreateMutexA(NULL, FALSE, NULL);
    WaitForSingleObject(g_com_mutex, 500);

    uint8_t frame[260];
    frame[0] = 0xAA;
    frame[1] = cmd;
    frame[2] = argLen;
    if (argLen) memcpy(frame + 3, args, argLen);
    frame[3 + argLen] = crc8_bridge(frame, 3 + argLen);
    DWORD wr = 0;
    WriteFile(g_com, frame, 4 + argLen, &wr, NULL);

    /* Receive response: 0x55 status len ... crc */
    uint8_t rx[260];
    DWORD rd = 0;
    /* First 3 bytes header */
    int got = 0;
    while (got < 3) {
        DWORD n;
        if (!ReadFile(g_com, rx + got, 3 - got, &n, NULL) || n == 0) {
            ReleaseMutex(g_com_mutex);
            return -1;
        }
        got += n;
    }
    if (rx[0] != 0x55) { ReleaseMutex(g_com_mutex); return -1; }
    uint8_t status = rx[1];
    uint8_t rlen   = rx[2];
    while (got < 3 + rlen + 1) {
        DWORD n;
        if (!ReadFile(g_com, rx + got, 3 + rlen + 1 - got, &n, NULL) || n == 0) break;
        got += n;
    }
    if (got < 3 + rlen + 1) { ReleaseMutex(g_com_mutex); return -1; }
    uint8_t calc_crc = crc8_bridge(rx, 3 + rlen);
    if (calc_crc != rx[3 + rlen]) { ReleaseMutex(g_com_mutex); return -1; }

    if (resp_status) *resp_status = status;
    int copy = rlen < resp_max ? rlen : resp_max;
    if (resp && copy) memcpy(resp, rx + 3, copy);
    ReleaseMutex(g_com_mutex);
    return copy;
}

static BOOL bridge_ping(void) {
    uint8_t s = 0, r[8];
    int n = bridge_cmd(0x10, NULL, 0, &s, r, 8);
    return (n >= 4 && s == 0 && r[0] == 'P' && r[1] == 'O' && r[2] == 'N' && r[3] == 'G');
}

typedef DWORD HID_SMBUS_STATUS;
#define HID_SMBUS_SUCCESS 0x00
#define HID_SMBUS_INVALID_DEVICE_OBJECT 0x02

typedef void* HID_SMBUS_DEVICE;

static void log_line(const char *fmt, ...) {
    char buf[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = wsprintfA(buf, "[%02d:%02d:%02d.%03d] ",
                      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    n += wvsprintfA(buf + n, fmt, ap);
    va_end(ap);
    buf[n++] = '\r'; buf[n++] = '\n'; buf[n] = 0;

    WaitForSingleObject(log_mutex, 1000);
    HANDLE h = CreateFileA(LOG_PATH, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr;
        SetFilePointer(h, 0, NULL, FILE_END);
        WriteFile(h, buf, n, &wr, NULL);
        CloseHandle(h);
    }
    ReleaseMutex(log_mutex);
}

static void log_hex(const char *prefix, const BYTE *data, int len) {
    char buf[512];
    int n = wsprintfA(buf, "%s [%d] ", prefix, len);
    for (int i = 0; i < len && n < 500; i++) {
        n += wsprintfA(buf + n, "%02X ", data[i]);
    }
    log_line("%s", buf);
}

/* Fake state */
static BOOL g_opened = FALSE;
static BYTE g_lastReadData[256] = {0};
static WORD g_lastReadLen = 0;
static BYTE g_lastReadSlave = 0;
static BYTE g_lastWriteTarget[2] = {0};
static BYTE g_lastWriteTargetLen = 0;

/* ============= CRITICAL EXPORTS ============= */

HID_SMBUS_STATUS __stdcall HidSmbus_GetNumDevices(DWORD *numDevices, WORD vid, WORD pid) {
    if (numDevices) *numDevices = 1;
    log_line("GetNumDevices(vid=0x%04X, pid=0x%04X) -> 1", vid, pid);
    return HID_SMBUS_SUCCESS;
}

HID_SMBUS_STATUS __stdcall HidSmbus_Open(HID_SMBUS_DEVICE *device, DWORD deviceNum, WORD vid, WORD pid) {
    if (device) *device = (HID_SMBUS_DEVICE)0x1234CAFE;
    g_opened = TRUE;
    log_line("Open(devNum=%u, vid=0x%04X, pid=0x%04X) -> OK, handle=0x1234CAFE", deviceNum, vid, pid);
    return HID_SMBUS_SUCCESS;
}

HID_SMBUS_STATUS __stdcall HidSmbus_Close(HID_SMBUS_DEVICE device) {
    g_opened = FALSE;
    log_line("Close(h=%p)", device);
    return HID_SMBUS_SUCCESS;
}

HID_SMBUS_STATUS __stdcall HidSmbus_IsOpened(HID_SMBUS_DEVICE device, BOOL *opened) {
    if (opened) *opened = g_opened;
    return HID_SMBUS_SUCCESS;
}

HID_SMBUS_STATUS __stdcall HidSmbus_GetAttributes(HID_SMBUS_DEVICE device, WORD *vid, WORD *pid, WORD *release) {
    if (vid) *vid = 0x10C4;
    if (pid) *pid = 0xEA90;
    if (release) *release = 0x0100;
    return HID_SMBUS_SUCCESS;
}

HID_SMBUS_STATUS __stdcall HidSmbus_GetOpenedAttributes(HID_SMBUS_DEVICE device, WORD *vid, WORD *pid, WORD *release) {
    return HidSmbus_GetAttributes(device, vid, pid, release);
}

HID_SMBUS_STATUS __stdcall HidSmbus_SetSmbusConfig(HID_SMBUS_DEVICE device, DWORD bitRate, BYTE address,
                                                    BOOL autoReadRespond, WORD writeTimeout,
                                                    WORD readTimeout, BOOL sclLowTimeout, WORD transferRetries) {
    log_line("SetSmbusConfig(rate=%u, addr=0x%02X, autoRead=%d, wrT=%u, rdT=%u, sclLow=%d, retries=%u)",
             bitRate, address, autoReadRespond, writeTimeout, readTimeout, sclLowTimeout, transferRetries);
    return HID_SMBUS_SUCCESS;
}

HID_SMBUS_STATUS __stdcall HidSmbus_GetSmbusConfig(HID_SMBUS_DEVICE device, DWORD *bitRate, BYTE *address,
                                                    BOOL *autoReadRespond, WORD *writeTimeout,
                                                    WORD *readTimeout, BOOL *sclLowTimeout, WORD *transferRetries) {
    if (bitRate) *bitRate = 100000;
    if (address) *address = 0x02;
    if (autoReadRespond) *autoReadRespond = 0;
    if (writeTimeout) *writeTimeout = 1000;
    if (readTimeout) *readTimeout = 1000;
    if (sclLowTimeout) *sclLowTimeout = 0;
    if (transferRetries) *transferRetries = 0;
    return HID_SMBUS_SUCCESS;
}

HID_SMBUS_STATUS __stdcall HidSmbus_SetTimeouts(HID_SMBUS_DEVICE device, WORD respT, WORD xferT) {
    return HID_SMBUS_SUCCESS;
}
HID_SMBUS_STATUS __stdcall HidSmbus_GetTimeouts(HID_SMBUS_DEVICE device, WORD *respT, WORD *xferT) {
    if (respT) *respT = 1000;
    if (xferT) *xferT = 1000;
    return HID_SMBUS_SUCCESS;
}

HID_SMBUS_STATUS __stdcall HidSmbus_WriteRequest(HID_SMBUS_DEVICE device, BYTE slaveAddress,
                                                  BYTE *buffer, BYTE numBytesToWrite) {
    char prefix[64];
    wsprintfA(prefix, "WriteRequest(slave=0x%02X) bytes:", slaveAddress);
    log_hex(prefix, buffer, numBytesToWrite);

    /* Forward to ESP32: CMD_WRITE args = [slave7, reg, len-1, data...] */
    if (g_bridge_active && numBytesToWrite >= 1) {
        BYTE slave7 = slaveAddress >> 1;
        BYTE reg    = buffer[0];
        BYTE dLen   = (numBytesToWrite > 1) ? numBytesToWrite - 1 : 0;
        /* Strip trailing PEC — last byte is PEC added by Battery Killer */
        if (dLen > 0) dLen--;
        uint8_t args[260];
        args[0] = slave7;
        args[1] = reg;
        args[2] = dLen;
        if (dLen > 0) memcpy(args + 3, buffer + 1, dLen);
        uint8_t status = 0, resp[8];
        int n = bridge_cmd(0x01, args, 3 + dLen, &status, resp, 8);
        log_line("  → bridge WRITE: slave7=0x%02X reg=0x%02X dLen=%u → status=0x%02X (bridge_n=%d)",
                 slave7, reg, dLen, status, n);
    }
    return HID_SMBUS_SUCCESS;
}

HID_SMBUS_STATUS __stdcall HidSmbus_ReadRequest(HID_SMBUS_DEVICE device, BYTE slaveAddress, WORD numBytesToRead) {
    log_line("ReadRequest(slave=0x%02X, n=%u)", slaveAddress, numBytesToRead);
    g_lastReadSlave = slaveAddress;
    g_lastReadLen = numBytesToRead;
    /* Fake fill: return zeros (may cause BK to bail — try FF if zeros fail) */
    memset(g_lastReadData, 0x00, sizeof(g_lastReadData));
    return HID_SMBUS_SUCCESS;
}

HID_SMBUS_STATUS __stdcall HidSmbus_AddressReadRequest(HID_SMBUS_DEVICE device, BYTE slaveAddress,
                                                        WORD numBytesToRead, BYTE targetAddressSize,
                                                        BYTE *targetAddress) {
    char prefix[80];
    wsprintfA(prefix, "AddressReadRequest(slave=0x%02X, n=%u, tSize=%u) target:",
              slaveAddress, numBytesToRead, targetAddressSize);
    log_hex(prefix, targetAddress, targetAddressSize);
    g_lastReadSlave = slaveAddress;
    g_lastReadLen = numBytesToRead;
    if (targetAddressSize <= 2 && targetAddress) {
        memcpy(g_lastWriteTarget, targetAddress, targetAddressSize);
        g_lastWriteTargetLen = targetAddressSize;
    }
    memset(g_lastReadData, 0x00, sizeof(g_lastReadData));

    /* Forward to ESP32: CMD_ADDR_READ args = [slave7, targetLen, target..., readLen] */
    if (g_bridge_active && targetAddressSize > 0 && targetAddress) {
        BYTE slave7 = slaveAddress >> 1;
        uint8_t args[260];
        args[0] = slave7;
        args[1] = targetAddressSize;
        memcpy(args + 2, targetAddress, targetAddressSize);
        args[2 + targetAddressSize] = (numBytesToRead > 250) ? 250 : (BYTE)numBytesToRead;
        uint8_t status = 0, resp[256];
        int n = bridge_cmd(0x05, args, 3 + targetAddressSize, &status, resp, 256);
        if (n >= 0 && status == 0) {
            int copy = n < (int)sizeof(g_lastReadData) ? n : (int)sizeof(g_lastReadData);
            memcpy(g_lastReadData, resp, copy);
            g_lastReadLen = copy;
            log_line("  → bridge ADDR_READ real: %d bytes from battery", n);
        } else {
            log_line("  → bridge ADDR_READ failed: status=0x%02X n=%d", status, n);
        }
    }
    return HID_SMBUS_SUCCESS;
}

HID_SMBUS_STATUS __stdcall HidSmbus_GetReadResponse(HID_SMBUS_DEVICE device, BYTE *status,
                                                     BYTE *buffer, BYTE bufferSize, BYTE *numBytesRead) {
    if (status) *status = 0x02; /* TRANSFER COMPLETE */
    BYTE n = g_lastReadLen > bufferSize ? bufferSize : (BYTE)g_lastReadLen;
    /* Return plausible "unsealed" state for block reads at 0x44:
       format expected: [len, d0, d1, d2, d3, d4, ...] where d4 has SEC bits.
       Set d4 = 0x02 (xx10 = Unsealed per Battery Killer's check) */
    if (buffer && n > 0) {
        memset(buffer, 0, n);
        if (n > 4) buffer[4] = 0x03;  /* Sealed → triggers Unseal attempt */
        if (n > 0) buffer[0] = (BYTE)(n > 1 ? n - 1 : 0);
    }
    if (numBytesRead) *numBytesRead = n;
    log_line("GetReadResponse -> %u bytes (status=COMPLETE)", n);
    return HID_SMBUS_SUCCESS;
}

HID_SMBUS_STATUS __stdcall HidSmbus_ForceReadResponse(HID_SMBUS_DEVICE device, WORD numBytesToRead) {
    log_line("ForceReadResponse(%u)", numBytesToRead);
    return HID_SMBUS_SUCCESS;
}

HID_SMBUS_STATUS __stdcall HidSmbus_TransferStatusRequest(HID_SMBUS_DEVICE device) {
    return HID_SMBUS_SUCCESS;
}

HID_SMBUS_STATUS __stdcall HidSmbus_GetTransferStatusResponse(HID_SMBUS_DEVICE device,
                                                                BYTE *status0, BYTE *status1,
                                                                WORD *numRetries, WORD *bytesRead) {
    if (status0) *status0 = 0x02;  /* TRANSFER COMPLETE */
    if (status1) *status1 = 0x00;  /* no error */
    if (numRetries) *numRetries = 0;
    if (bytesRead) *bytesRead = g_lastReadLen;
    return HID_SMBUS_SUCCESS;
}

HID_SMBUS_STATUS __stdcall HidSmbus_Reset(HID_SMBUS_DEVICE device) {
    log_line("Reset");
    return HID_SMBUS_SUCCESS;
}

HID_SMBUS_STATUS __stdcall HidSmbus_CancelIo(HID_SMBUS_DEVICE device) { return HID_SMBUS_SUCCESS; }
HID_SMBUS_STATUS __stdcall HidSmbus_CancelTransfer(HID_SMBUS_DEVICE device) { return HID_SMBUS_SUCCESS; }

/* ============= LESS CRITICAL — stub to success ============= */

HID_SMBUS_STATUS __stdcall HidSmbus_GetLibraryVersion(BYTE *major, BYTE *minor, BOOL *release) {
    if (major) *major = 2; if (minor) *minor = 2; if (release) *release = TRUE;
    return HID_SMBUS_SUCCESS;
}
HID_SMBUS_STATUS __stdcall HidSmbus_GetHidLibraryVersion(BYTE *major, BYTE *minor, BOOL *release) {
    if (major) *major = 2; if (minor) *minor = 2; if (release) *release = TRUE;
    return HID_SMBUS_SUCCESS;
}
HID_SMBUS_STATUS __stdcall HidSmbus_GetHidGuid(void *guid) { return HID_SMBUS_SUCCESS; }

#define STR_STUB(name) HID_SMBUS_STATUS __stdcall name(DWORD a, WORD b, WORD c, BYTE *s) { \
    if (s) strcpy((char*)s, "Fake CP2112"); return HID_SMBUS_SUCCESS; }
STR_STUB(HidSmbus_GetString)
STR_STUB(HidSmbus_GetIndexedString)

HID_SMBUS_STATUS __stdcall HidSmbus_GetOpenedString(HID_SMBUS_DEVICE d, BYTE *s, DWORD t) {
    if (s) strcpy((char*)s, "Fake CP2112"); return HID_SMBUS_SUCCESS;
}
HID_SMBUS_STATUS __stdcall HidSmbus_GetOpenedIndexedString(HID_SMBUS_DEVICE d, DWORD i, BYTE *s) {
    if (s) strcpy((char*)s, "Fake"); return HID_SMBUS_SUCCESS;
}
HID_SMBUS_STATUS __stdcall HidSmbus_GetPartNumber(HID_SMBUS_DEVICE d, BYTE *pn, BYTE *ver) {
    if (pn) *pn = 0x0B; if (ver) *ver = 0x01; return HID_SMBUS_SUCCESS;
}
HID_SMBUS_STATUS __stdcall HidSmbus_GetManufacturingString(DWORD a, WORD b, WORD c, BYTE *s) {
    if (s) strcpy((char*)s, "Silicon Labs"); return HID_SMBUS_SUCCESS;
}
HID_SMBUS_STATUS __stdcall HidSmbus_SetManufacturingString(HID_SMBUS_DEVICE d, BYTE *s) { return HID_SMBUS_SUCCESS; }
HID_SMBUS_STATUS __stdcall HidSmbus_GetProductString(DWORD a, WORD b, WORD c, BYTE *s) {
    if (s) strcpy((char*)s, "CP2112 HID USB-to-SMBus Bridge"); return HID_SMBUS_SUCCESS;
}
HID_SMBUS_STATUS __stdcall HidSmbus_SetProductString(HID_SMBUS_DEVICE d, BYTE *s) { return HID_SMBUS_SUCCESS; }
HID_SMBUS_STATUS __stdcall HidSmbus_GetSerialString(DWORD a, WORD b, WORD c, BYTE *s) {
    if (s) strcpy((char*)s, "FAKE001"); return HID_SMBUS_SUCCESS;
}
HID_SMBUS_STATUS __stdcall HidSmbus_SetSerialString(HID_SMBUS_DEVICE d, BYTE *s) { return HID_SMBUS_SUCCESS; }

HID_SMBUS_STATUS __stdcall HidSmbus_GetGpioConfig(HID_SMBUS_DEVICE d, BYTE *dir, BYTE *mode, BYTE *special, BYTE *div) {
    if (dir) *dir = 0; if (mode) *mode = 0; if (special) *special = 0; if (div) *div = 0;
    return HID_SMBUS_SUCCESS;
}
HID_SMBUS_STATUS __stdcall HidSmbus_SetGpioConfig(HID_SMBUS_DEVICE d, BYTE a, BYTE b, BYTE c, BYTE e) { return HID_SMBUS_SUCCESS; }
HID_SMBUS_STATUS __stdcall HidSmbus_ReadLatch(HID_SMBUS_DEVICE d, BYTE *val) { if (val) *val = 0xFF; return HID_SMBUS_SUCCESS; }
HID_SMBUS_STATUS __stdcall HidSmbus_WriteLatch(HID_SMBUS_DEVICE d, BYTE v, BYTE m) { return HID_SMBUS_SUCCESS; }
HID_SMBUS_STATUS __stdcall HidSmbus_GetLock(HID_SMBUS_DEVICE d, BYTE *lock) { if (lock) *lock = 0; return HID_SMBUS_SUCCESS; }
HID_SMBUS_STATUS __stdcall HidSmbus_SetLock(HID_SMBUS_DEVICE d, BYTE lock) { return HID_SMBUS_SUCCESS; }
HID_SMBUS_STATUS __stdcall HidSmbus_GetUsbConfig(HID_SMBUS_DEVICE d, WORD *v, WORD *p, WORD *r, BYTE *m, BYTE *pw, BYTE *m2) {
    if (v) *v = 0x10C4; if (p) *p = 0xEA90;
    return HID_SMBUS_SUCCESS;
}
HID_SMBUS_STATUS __stdcall HidSmbus_SetUsbConfig(HID_SMBUS_DEVICE d, WORD v, WORD p, WORD r, BYTE m, BYTE pw, BYTE m2, BYTE m3) { return HID_SMBUS_SUCCESS; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        if (!log_mutex) log_mutex = CreateMutexA(NULL, FALSE, "Global\\SMBusShimLog");
        if (!g_com_mutex) g_com_mutex = CreateMutexA(NULL, FALSE, NULL);
        log_line("========== SHIM DLL LOADED (PID=%u) ==========", GetCurrentProcessId());
        /* Try to activate bridge by pinging ESP32 on COM5 */
        if (bridge_ping()) {
            g_bridge_active = TRUE;
            log_line("*** BRIDGE ACTIVE: ESP32 responded PONG on %s ***", COM_PORT);
        } else {
            log_line("Bridge inactive: %s not responding — fallback to log-only mode", COM_PORT);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_com != INVALID_HANDLE_VALUE) CloseHandle(g_com);
    }
    return TRUE;
}
