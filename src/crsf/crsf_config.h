#pragma once
#include <Arduino.h>
#include "crsf_proto.h"

namespace CRSFConfig {

// Parameter entry (reconstructed from chunked responses)
struct Param {
    uint8_t  id;
    uint8_t  parent_id;
    uint8_t  type;          // PARAM_* from crsf_proto
    bool     hidden;
    String   name;
    String   value_text;    // for TEXT_SELECTION, STRING, INFO
    int32_t  value_num;     // for numeric types
    int32_t  min_val;
    int32_t  max_val;
    int32_t  default_val;
    String   unit;
    String   options;       // semicolon-separated options for TEXT_SELECTION
    uint8_t  option_index;  // for TEXT_SELECTION current selection
    bool     complete;      // received all chunks
};

// Device info (from 0x29 response)
struct DeviceInfo {
    String   name;
    uint32_t serial;
    uint32_t hw_ver;
    uint32_t sw_ver;
    uint8_t  field_count;   // number of parameters
    uint8_t  param_proto_version;
    bool     valid;
};

// Init — must be called after CRSFService begin
void init();

// Process incoming extended frames (called from CRSFService)
void handleFrame(uint8_t type, const uint8_t *payload, uint8_t payload_len);

// Request device info (ping)
void requestDeviceInfo();

// Request all parameters
void requestAllParameters();

// Request specific parameter (will handle chunking automatically)
void requestParameter(uint8_t id);

// Write parameter value
bool writeParamByte(uint8_t id, uint8_t value);
bool writeParamText(uint8_t id, const String &value);

// Accessors
const DeviceInfo& deviceInfo();
int paramCount();
const Param& param(int index);
const Param* paramById(uint8_t id);

// Find first COMMAND-type parameter whose name contains substring (case-insensitive).
// Used to locate ELRS "Enable WiFi" action without hardcoding IDs.
const Param* findCommandParamByName(const char* substr);

// Call from loop() — handles timeouts and chunked receives
void loop();

// Clear all stored parameters
void reset();

} // namespace CRSFConfig
