#include "dshot.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

// DShot frame: 16 bits = 11 throttle + 1 telemetry + 4 CRC
// Bit 1: ~75% high, ~25% low
// Bit 0: ~37% high, ~63% low

static rmt_channel_handle_t s_channel = nullptr;
static rmt_encoder_handle_t s_copy_encoder = nullptr;
static uint8_t s_pin = 0;
static DShotSpeed s_speed = DSHOT300;
static bool s_initialized = false;

static uint16_t s_t1h_ticks, s_t1l_ticks;
static uint16_t s_t0h_ticks, s_t0l_ticks;

static const uint32_t RMT_RESOLUTION_HZ = 40000000; // 40MHz

static void calcTiming(DShotSpeed speed) {
    uint32_t bit_ns = 1000000000UL / ((uint32_t)speed * 1000);
    uint32_t tick_ns = 1000000000UL / RMT_RESOLUTION_HZ;

    s_t1h_ticks = (bit_ns * 3 / 4) / tick_ns;
    s_t1l_ticks = bit_ns / tick_ns - s_t1h_ticks;
    s_t0h_ticks = (bit_ns * 3 / 8) / tick_ns;
    s_t0l_ticks = bit_ns / tick_ns - s_t0h_ticks;
}

static uint8_t dshotCRC(uint16_t packet) {
    uint16_t csum = 0;
    uint16_t data = packet;
    for (int i = 0; i < 3; i++) {
        csum ^= data;
        data >>= 4;
    }
    return csum & 0x0F;
}

static uint16_t buildFrame(uint16_t throttle, bool telemetry) {
    uint16_t packet = (throttle << 1) | (telemetry ? 1 : 0);
    return (packet << 4) | dshotCRC(packet);
}

// Pre-encode frame into RMT symbols buffer
static rmt_symbol_word_t s_symbols[16];

static void encodeFrame(uint16_t frame) {
    for (int i = 0; i < 16; i++) {
        bool bit = (frame >> (15 - i)) & 1; // MSB first
        s_symbols[i].duration0 = bit ? s_t1h_ticks : s_t0h_ticks;
        s_symbols[i].level0 = 1;
        s_symbols[i].duration1 = bit ? s_t1l_ticks : s_t0l_ticks;
        s_symbols[i].level1 = 0;
    }
}

bool DShot::init(uint8_t pin, DShotSpeed speed) {
    // Clean up previous if any
    if (s_initialized) stop();

    s_pin = pin;
    s_speed = speed;
    calcTiming(speed);

    rmt_tx_channel_config_t tx_config = {};
    tx_config.gpio_num = (gpio_num_t)pin;
    tx_config.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_config.resolution_hz = RMT_RESOLUTION_HZ;
    tx_config.mem_block_symbols = 64;
    tx_config.trans_queue_depth = 1;

    esp_err_t err = rmt_new_tx_channel(&tx_config, &s_channel);
    if (err != ESP_OK) {
        Serial.printf("DShot: RMT channel init failed: %d\n", err);
        s_channel = nullptr;
        return false;
    }

    rmt_copy_encoder_config_t copy_config = {};
    err = rmt_new_copy_encoder(&copy_config, &s_copy_encoder);
    if (err != ESP_OK) {
        Serial.printf("DShot: encoder init failed: %d\n", err);
        rmt_del_channel(s_channel);
        s_channel = nullptr;
        return false;
    }

    err = rmt_enable(s_channel);
    if (err != ESP_OK) {
        Serial.printf("DShot: channel enable failed: %d\n", err);
        rmt_del_encoder(s_copy_encoder);
        rmt_del_channel(s_channel);
        s_channel = nullptr;
        s_copy_encoder = nullptr;
        return false;
    }

    s_initialized = true;
    Serial.printf("DShot: init OK, %s on GPIO%d\n",
        speed == DSHOT150 ? "DShot150" : speed == DSHOT300 ? "DShot300" : "DShot600", pin);
    return true;
}

void DShot::stop() {
    if (s_channel) {
        rmt_disable(s_channel);
        rmt_del_channel(s_channel);
        s_channel = nullptr;
    }
    if (s_copy_encoder) {
        rmt_del_encoder(s_copy_encoder);
        s_copy_encoder = nullptr;
    }
    s_initialized = false;
}

void DShot::sendThrottle(uint16_t throttle, bool telemetry) {
    if (!s_initialized || !s_channel || !s_copy_encoder) return;
    throttle = constrain(throttle, 0, 2047);

    uint16_t frame = buildFrame(throttle, telemetry);
    encodeFrame(frame);

    rmt_transmit_config_t tx_config = {};
    tx_config.loop_count = 0;

    // Send pre-encoded symbols directly via copy encoder
    rmt_transmit(s_channel, s_copy_encoder, s_symbols,
                 sizeof(rmt_symbol_word_t) * 16, &tx_config);
    // Non-blocking wait with 10ms timeout (won't freeze UI)
    rmt_tx_wait_all_done(s_channel, pdMS_TO_TICKS(10));
}

void DShot::sendCommand(uint8_t cmd) {
    if (cmd > 47) return;
    // DShot commands are sent as throttle values 1-47 with telemetry=1
    // Must be sent 10+ times for ESC to accept
    for (int i = 0; i < 12; i++) {
        sendThrottle(cmd, true);
        delayMicroseconds(1500);
    }
}

void DShot::arm() {
    // Send throttle=0 for ~500ms to arm ESC
    for (int i = 0; i < 250; i++) {
        sendThrottle(0, false);
        delayMicroseconds(2000);
    }
}
