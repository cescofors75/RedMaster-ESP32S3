#pragma once

#include <Arduino.h>

namespace cfg {

// Keep the real-time path free of Serial backpressure. Enable temporarily while
// diagnosing a wiring issue.
static constexpr bool kDebugLog = false;

// WiFi / UDP (igual que P4)
static constexpr const char* kWifiSsid = "RED808";
static constexpr const char* kWifiPass = "red808esp32";
static constexpr const char* kMasterIp = "192.168.4.1";
static constexpr uint16_t kMasterUdpPort = 8888;
static constexpr uint16_t kLocalUdpPort = 8890;

// Tiempos de red
static constexpr uint32_t kWifiReconnectMs = 2500;
static constexpr uint32_t kWifiConnectTimeoutMs = 10000;
// A recognised "hello" keeps the UDP client registered and refreshes state.
static constexpr uint32_t kHeartbeatMs = 10000;
static constexpr size_t kUdpMaxPacketBytes = 4096;
static constexpr uint8_t kMasterPatternCount = 128;

// I2C base
static constexpr uint8_t kI2cSdaPin = 7;
static constexpr uint8_t kI2cSclPin = 8;
static constexpr uint32_t kI2cClockHz = 400000;  // Fast mode (was 100kHz)
static constexpr uint8_t kI2cHubAddr = 0x70; // PCA9548A
static constexpr uint32_t kI2cHubSettleUs = 8;
static constexpr uint32_t kI2cTimeoutMs = 20;
static constexpr bool kI2cBaseIsolationMode = false;

// Direcciones I2C de modulos
static constexpr uint8_t kAddrDfRobotRotary = 0x54;
static constexpr uint8_t kAddrM5Encoder8 = 0x41;
static constexpr uint8_t kAddrM5ByteButton = 0x47;
static constexpr bool kEnableDfRotary = true;
// M5 hardware is not installed yet. Keeping these disabled avoids four empty
// hub probes on every input cycle.
static constexpr bool kEnableM5Encoder = false;
static constexpr bool kEnableM5ByteButton = false;

static constexpr float kTempoMin = 60.0f;
static constexpr float kTempoMax = 200.0f;

// Polling
static constexpr bool kEnableI2cPolling = true;
static constexpr uint32_t kInputPollMs = 8;
static constexpr uint32_t kFaderPollMs = 15;

// Rotary encoder quality
// Gain: higher = more raw units per physical click = more responsive.
// The visual smoothness comes from the P4 display lerp, NOT from EMA here.
static constexpr uint8_t  kRotaryGainCoeff        = 20;    // was 5 (too slow), 20 = ~2% FX per click
static constexpr float    kRotarySmoothAlpha       = 1.0f;  // 1.0 = NO EMA (raw value, P4 lerps)
static constexpr uint32_t kRotaryMinSendIntervalMs = 50;    // 20Hz max UDP rate per encoder

// Interrupt-driven I2C wake (optional, requires INT pins wired to GPIOs).
// Set kEnableDfRotaryInterrupt=true and fill kDfRotaryIntPins with real GPIO numbers.
// While disabled, the task falls back to kInputPollMs timeout.
static constexpr bool    kEnableDfRotaryInterrupt  = false;
static constexpr int8_t  kDfRotaryIntPins[4]       = {-1, -1, -1, -1}; // GPIO per encoder

// Fader unit analog
static constexpr bool kEnableFaderAnalog = false;
static constexpr uint8_t kFaderAnalogPin = 0;

// Reserva fase 2 (rotary analog directos)
static constexpr uint8_t kReservedAnalogRotaryPins[4] = {1, 2, 3, 4};

} // namespace cfg
