#include "crsf_config.h"
#include "crsf_service.h"

namespace CRSFConfig {

static const int MAX_PARAMS = 64;
static Param s_params[MAX_PARAMS];
static int s_param_count = 0;
static DeviceInfo s_device = {};

// Chunk buffer for reassembling multi-chunk parameter entries
static uint8_t s_current_param_id = 0xFF;
static uint8_t s_expected_chunks = 0;
static uint8_t s_received_chunks = 0;
static uint8_t s_chunk_buf[512];
static size_t  s_chunk_buf_len = 0;
static uint32_t s_last_request_ms = 0;

// Deferred re-read after a write so the cached Param refreshes with
// whatever the RX actually accepted (clamp, reject, etc).
static uint8_t  s_reread_id = 0;
static uint32_t s_reread_at_ms = 0;

// ---------- Helpers ----------
static Param* findOrCreateParam(uint8_t id) {
    for (int i = 0; i < s_param_count; i++) {
        if (s_params[i].id == id) return &s_params[i];
    }
    if (s_param_count >= MAX_PARAMS) return nullptr;
    Param *p = &s_params[s_param_count++];
    memset(p, 0, sizeof(Param));
    p->id = id;
    return p;
}

// Read null-terminated string from buffer, advance pointer
static String readCString(const uint8_t **p, const uint8_t *end) {
    const uint8_t *s = *p;
    while (*p < end && **p != 0) (*p)++;
    String res((const char*)s);
    if (*p < end) (*p)++;  // skip null
    return res;
}

static uint32_t readU32BE(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

// ---------- Public API ----------
void init() {
    reset();
}

void reset() {
    s_param_count = 0;
    memset(&s_device, 0, sizeof(s_device));
    s_current_param_id = 0xFF;
    s_chunk_buf_len = 0;
    s_reread_id = 0;
}

void requestDeviceInfo() {
    CRSFService::sendDevicePing();
    s_last_request_ms = millis();
}

// Handle FRAME_DEVICE_INFO (0x29)
static void handleDeviceInfo(const uint8_t *payload, uint8_t len) {
    // [dest] [origin] [name...\0] [serial(4)] [hw_ver(4)] [sw_ver(4)] [field_count] [param_proto]
    if (len < 4) return;
    const uint8_t *p = payload + 2;  // skip dest+origin
    const uint8_t *end = payload + len;

    s_device.name = readCString(&p, end);
    if (end - p >= 4) { s_device.serial = readU32BE(p); p += 4; }
    if (end - p >= 4) { s_device.hw_ver = readU32BE(p); p += 4; }
    if (end - p >= 4) { s_device.sw_ver = readU32BE(p); p += 4; }
    if (end - p >= 2) {
        s_device.field_count = *p++;
        s_device.param_proto_version = *p++;
    }
    s_device.valid = true;
    Serial.printf("[CRSF] Device: %s, fw=0x%08X, fields=%d\n",
        s_device.name.c_str(), s_device.sw_ver, s_device.field_count);
}

// Handle FRAME_PARAMETER_SETTINGS_ENTRY (0x2B)
// Chunked: [dest][origin][param_id][chunks_remaining][chunk_data...]
static void handleParamEntry(const uint8_t *payload, uint8_t len) {
    if (len < 4) return;
    uint8_t param_id = payload[2];
    uint8_t chunks_remaining = payload[3];
    const uint8_t *chunk = payload + 4;
    size_t chunk_len = len - 4;

    if (param_id != s_current_param_id) {
        // New param — reset buffer
        s_current_param_id = param_id;
        s_chunk_buf_len = 0;
    }

    if (s_chunk_buf_len + chunk_len > sizeof(s_chunk_buf)) {
        s_chunk_buf_len = 0;
        return;
    }
    memcpy(s_chunk_buf + s_chunk_buf_len, chunk, chunk_len);
    s_chunk_buf_len += chunk_len;

    if (chunks_remaining > 0) {
        // Request next chunk
        // Format: FRAME_PARAMETER_READ [dest][origin][param_id][chunk_index]
        // For now, we wait for the RX to send more (some impls push all chunks)
        return;
    }

    // Last chunk — parse the full buffer
    Param *p = findOrCreateParam(param_id);
    if (!p) { s_chunk_buf_len = 0; return; }

    // Layout (standard CRSF parameter):
    //   [parent_id] [type(7bit) | hidden(1bit)] [name\0] [type-specific data]
    if (s_chunk_buf_len < 3) { s_chunk_buf_len = 0; return; }

    const uint8_t *ptr = s_chunk_buf;
    const uint8_t *end = s_chunk_buf + s_chunk_buf_len;

    p->parent_id = *ptr++;
    uint8_t type_byte = *ptr++;
    p->type = type_byte & 0x7F;
    p->hidden = (type_byte & 0x80) != 0;
    p->name = readCString(&ptr, end);

    // Type-specific parsing
    switch (p->type) {
        case CRSF::PARAM_UINT8:
        case CRSF::PARAM_INT8:
            if (end - ptr >= 4) {
                p->value_num = (int8_t)ptr[0];
                p->min_val   = (int8_t)ptr[1];
                p->max_val   = (int8_t)ptr[2];
                p->default_val = (int8_t)ptr[3];
                ptr += 4;
                p->unit = readCString(&ptr, end);
            }
            break;

        case CRSF::PARAM_UINT16:
        case CRSF::PARAM_INT16:
            if (end - ptr >= 8) {
                p->value_num = (int16_t)((ptr[0]<<8)|ptr[1]);
                p->min_val   = (int16_t)((ptr[2]<<8)|ptr[3]);
                p->max_val   = (int16_t)((ptr[4]<<8)|ptr[5]);
                p->default_val = (int16_t)((ptr[6]<<8)|ptr[7]);
                ptr += 8;
                p->unit = readCString(&ptr, end);
            }
            break;

        case CRSF::PARAM_TEXT_SELECTION: {
            // [options;sep;by;semicolon\0][value][min][max][default][unit\0]
            p->options = readCString(&ptr, end);
            if (end - ptr >= 4) {
                p->value_num = *ptr++;
                p->option_index = (uint8_t)p->value_num;
                p->min_val = *ptr++;
                p->max_val = *ptr++;
                p->default_val = *ptr++;
                p->unit = readCString(&ptr, end);
            }
            break;
        }

        case CRSF::PARAM_STRING:
        case CRSF::PARAM_INFO:
            p->value_text = readCString(&ptr, end);
            break;

        case CRSF::PARAM_FOLDER:
            // Name is enough; folder has no value
            break;

        case CRSF::PARAM_COMMAND:
            // [status][timeout][info\0]
            if (end - ptr >= 2) {
                p->value_num = *ptr++;
                p->max_val = *ptr++;  // timeout
                p->value_text = readCString(&ptr, end);
            }
            break;
    }

    p->complete = true;
    s_chunk_buf_len = 0;
    s_current_param_id = 0xFF;

    Serial.printf("[CRSF] Param %d: '%s' type=%d\n", p->id, p->name.c_str(), p->type);
}

void handleFrame(uint8_t type, const uint8_t *payload, uint8_t payload_len) {
    switch (type) {
        case CRSF::FRAME_DEVICE_INFO:
            handleDeviceInfo(payload, payload_len);
            break;
        case CRSF::FRAME_PARAMETER_SETTINGS_ENTRY:
            handleParamEntry(payload, payload_len);
            break;
        default:
            break;
    }
}

const DeviceInfo& deviceInfo() { return s_device; }
int paramCount() { return s_param_count; }
const Param& param(int index) { return s_params[index]; }

const Param* paramById(uint8_t id) {
    for (int i = 0; i < s_param_count; i++) {
        if (s_params[i].id == id) return &s_params[i];
    }
    return nullptr;
}

const Param* findCommandParamByName(const char* substr) {
    if (!substr) return nullptr;
    String needle(substr);
    needle.toLowerCase();
    for (int i = 0; i < s_param_count; i++) {
        const Param &p = s_params[i];
        if (!p.complete) continue;
        if (p.type != CRSF::PARAM_COMMAND) continue;
        String name = p.name;
        name.toLowerCase();
        if (name.indexOf(needle) >= 0) return &s_params[i];
    }
    return nullptr;
}

// Request all params — iterate through known field_count, request one by one
static uint8_t s_next_request_id = 0;
static uint32_t s_next_request_ms = 0;
static bool s_requesting_all = false;

void requestAllParameters() {
    s_requesting_all = true;
    s_next_request_id = 1;  // params are 1-indexed
    s_next_request_ms = millis();
}

void requestParameter(uint8_t id) {
    CRSFService::sendParameterRead(id, 0);
    s_current_param_id = id;
    s_last_request_ms = millis();
}

// After a successful write, schedule a re-read so the cached Param
// refreshes — ELRS usually echoes 0x2B anyway, but this closes the gap.
static void scheduleReread(uint8_t id) {
    s_reread_id = id;
    s_reread_at_ms = millis() + 150;
}

bool writeParamByte(uint8_t id, uint8_t value) {
    uint8_t v = value;
    bool ok = CRSFService::sendParameterWrite(id, &v, 1);
    if (ok) scheduleReread(id);
    return ok;
}

bool writeParamText(uint8_t id, const String &value) {
    bool ok = CRSFService::sendParameterWrite(id, (const uint8_t*)value.c_str(), value.length() + 1);
    if (ok) scheduleReread(id);
    return ok;
}

bool writeParamInt(uint8_t id, int32_t value) {
    const Param *p = paramById(id);
    if (!p) return false;
    uint8_t buf[4];
    uint8_t len = 0;
    switch (p->type) {
        case CRSF::PARAM_UINT8:
        case CRSF::PARAM_INT8:
            buf[0] = (uint8_t)(value & 0xFF);
            len = 1;
            break;
        case CRSF::PARAM_UINT16:
        case CRSF::PARAM_INT16: {
            uint16_t v = (uint16_t)(value & 0xFFFF);
            buf[0] = (v >> 8) & 0xFF;
            buf[1] = v & 0xFF;
            len = 2;
            break;
        }
        default:
            return false;
    }
    bool ok = CRSFService::sendParameterWrite(id, buf, len);
    if (ok) scheduleReread(id);
    return ok;
}

bool writeParamAuto(uint8_t id, const String &value) {
    const Param *p = paramById(id);
    if (!p) return false;
    switch (p->type) {
        case CRSF::PARAM_STRING:
            return writeParamText(id, value);
        case CRSF::PARAM_TEXT_SELECTION:
        case CRSF::PARAM_COMMAND:
        case CRSF::PARAM_UINT8:
        case CRSF::PARAM_INT8:
            return writeParamInt(id, value.toInt());
        case CRSF::PARAM_UINT16:
        case CRSF::PARAM_INT16:
            return writeParamInt(id, value.toInt());
        default:
            return false;  // FOLDER / INFO / FLOAT (unsupported) etc.
    }
}

void loop() {
    // Timeout check for chunked receives
    if (s_current_param_id != 0xFF &&
        millis() - s_last_request_ms > 1000) {
        s_current_param_id = 0xFF;
        s_chunk_buf_len = 0;
    }

    // Fire a deferred re-read after a write so the cached value refreshes.
    if (s_reread_id != 0 && (int32_t)(millis() - s_reread_at_ms) >= 0) {
        uint8_t id = s_reread_id;
        s_reread_id = 0;
        requestParameter(id);
    }

    // Batch parameter requesting
    if (s_requesting_all && s_device.valid && s_device.field_count > 0) {
        if (millis() - s_next_request_ms > 200) {
            if (s_next_request_id <= s_device.field_count) {
                const Param *p = paramById(s_next_request_id);
                if (!p || !p->complete) {
                    requestParameter(s_next_request_id);
                }
                s_next_request_id++;
                s_next_request_ms = millis();
            } else {
                s_requesting_all = false;
            }
        }
    }
}

} // namespace CRSFConfig
