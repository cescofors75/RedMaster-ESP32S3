#include "drivers/input_manager.h"

#include <Arduino.h>
#include <DFRobot_VisualRotaryEncoder.h>
#include <M5ROTATE8.h>
#include <Wire.h>
#if defined(ARDUINO_ARCH_ESP32)
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#endif

#include "config.h"
#include "device_map.h"
#include "drivers/i2c_driver.h"

namespace {
constexpr uint8_t kQueueLen = 64;
constexpr uint8_t kByteButtonStatusReg = 0x00;
constexpr uint8_t kByteButtonStatus8ByteReg = 0x60;
constexpr uint8_t kByteButtonButtons = 8;
constexpr uint8_t kByteButtonLedBrightnessReg = 0x10;
constexpr uint8_t kByteButtonLedShowModeReg = 0x19;
constexpr uint8_t kByteButtonLedRgb888Reg = 0x20;
constexpr uint8_t kByteButtonLedCount = kByteButtonButtons + 1;
constexpr uint8_t kByteButtonLedUserDefined = 0;
constexpr uint8_t kByteButtonBrightness = 160;
constexpr uint8_t kEncodersPerModule = 8;
constexpr int32_t kM5DeltaClamp = 8;
constexpr uint8_t kDfRotaryCount = 4;
constexpr uint16_t kDfRotaryCenter = 512;
constexpr uint16_t kDfRotaryStepThreshold = 2;
constexpr uint32_t kDfButtonGuardMs = 250;
#if defined(ARDUINO_ARCH_ESP32)
QueueHandle_t g_eventQueue = nullptr;
#else
InputEvent g_queue[kQueueLen] = {};
uint8_t g_head = 0;
uint8_t g_tail = 0;
#endif
volatile uint32_t g_droppedEvents = 0;
int16_t g_lastFader = -1;
bool g_hubDetected = false;
bool g_byteButton0Detected = false;
bool g_byteButton1Detected = false;
bool g_m5Encoder0Detected = false;
uint8_t g_prevByteButton0State = 0;
uint8_t g_prevByteButton1State = 0;
uint8_t g_byteButton0FailCount = 0;
uint8_t g_byteButton1FailCount = 0;
uint32_t g_byteButton0SuspendUntilMs = 0;
uint32_t g_byteButton1SuspendUntilMs = 0;
uint8_t g_byteButton0PresenceCount = 0;
uint8_t g_byteButton1PresenceCount = 0;
uint8_t g_m5Encoder0PresenceCount = 0;
uint8_t g_hubProbeChannel = 0;
uint32_t g_lastHubProbeLogMs = 0;
uint32_t g_hubRecoverUntilMs = 0;
uint32_t g_nextHubProbeMs = 0;
uint8_t g_hubRecoverCount = 0;
DFRobot_VisualRotaryEncoder_I2C* g_dfRotary[kDfRotaryCount] = {};
bool g_dfRotaryDetected[kDfRotaryCount] = {};
uint16_t g_prevDfRotaryValue[kDfRotaryCount] = {};
bool g_prevDfRotaryButtonDown[kDfRotaryCount] = {};
uint32_t g_lastDfButtonMs[kDfRotaryCount] = {};
float g_dfRotarySmoothed[kDfRotaryCount] = {   // EMA state (initialized in init)
  kDfRotaryCenter, kDfRotaryCenter, kDfRotaryCenter, kDfRotaryCenter
};
uint32_t g_lastDfRotaryEventMs[kDfRotaryCount] = {};
#if defined(ARDUINO_ARCH_ESP32)
static SemaphoreHandle_t s_i2c_sem = nullptr;
#endif
int32_t g_prevM5Counter0[kEncodersPerModule] = {};
uint8_t g_m5ButtonConfirm0[kEncodersPerModule] = {};
bool g_m5ButtonArmed0[kEncodersPerModule] = {};
unsigned long g_lastM5ButtonPress0[kEncodersPerModule] = {};
uint32_t g_lastLedAnimMs = 0;
uint8_t g_bootAnimPhase = 0;
bool g_bootAnimDone = false;
bool g_byteButton0LedsReady = false;
bool g_byteButton1LedsReady = false;
M5ROTATE8 g_m5Encoder0;
bool g_m5Encoder1Detected = false;
uint8_t g_m5Encoder1PresenceCount = 0;
int32_t g_prevM5Counter1[kEncodersPerModule] = {};
uint8_t g_m5ButtonConfirm1[kEncodersPerModule] = {};
bool g_m5ButtonArmed1[kEncodersPerModule] = {};
unsigned long g_lastM5ButtonPress1[kEncodersPerModule] = {};
M5ROTATE8 g_m5Encoder1;

// Pending LED update requests (written by udp_handler task, read by I2C task)
struct LedRequest { uint8_t r, g, b; bool dirty; };
static LedRequest g_pendingLed0[8] = {};
static LedRequest g_pendingLed1[8] = {};
struct EncLedRequest { uint8_t r, g, b; bool dirty; };
static EncLedRequest g_pendingEncLed0[8] = {};
static EncLedRequest g_pendingEncLed1[8] = {};
#if defined(ARDUINO_ARCH_ESP32)
portMUX_TYPE g_ledMux = portMUX_INITIALIZER_UNLOCKED;
#endif

bool push_event(uint8_t id, uint8_t elementId, int16_t value, uint8_t type) {
#if defined(ARDUINO_ARCH_ESP32)
  const InputEvent event{id, elementId, value, type};
  if (!g_eventQueue || xQueueSend(g_eventQueue, &event, 0) != pdTRUE) {
    __atomic_fetch_add(&g_droppedEvents, 1, __ATOMIC_RELAXED);
    return false;
  }
  return true;
#else
  uint8_t next = static_cast<uint8_t>((g_head + 1) % kQueueLen);
  if (next == g_tail) {
    ++g_droppedEvents;
    return false;
  }
  g_queue[g_head] = {id, elementId, value, type};
  g_head = next;
  return true;
#endif
}

template <typename Request>
bool take_pending_led(Request* requests, uint8_t index, Request& out) {
#if defined(ARDUINO_ARCH_ESP32)
  portENTER_CRITICAL(&g_ledMux);
#endif
  bool available = requests[index].dirty;
  if (available) {
    out = requests[index];
    requests[index].dirty = false;
  }
#if defined(ARDUINO_ARCH_ESP32)
  portEXIT_CRITICAL(&g_ledMux);
#endif
  return available;
}

bool probe_selected_bus_device(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool detect_hub_locked() {
  Wire.beginTransmission(cfg::kI2cHubAddr);
  return Wire.endTransmission() == 0;
}

bool detect_df_rotary_locked(uint8_t rotaryIndex) {
  if (rotaryIndex >= kDfRotaryCount) return false;
  uint8_t hubChannel = static_cast<uint8_t>(devices::HUB_CH_DF_ROT_0 + rotaryIndex);
  if (!i2c_hub_select(hubChannel)) return false;
  if (!probe_selected_bus_device(cfg::kAddrDfRobotRotary)) {
    i2c_hub_deselect();
    return false;
  }

  if (!g_dfRotary[rotaryIndex]) {
    g_dfRotary[rotaryIndex] = new DFRobot_VisualRotaryEncoder_I2C(cfg::kAddrDfRobotRotary, &Wire);
    if (!g_dfRotary[rotaryIndex] || g_dfRotary[rotaryIndex]->begin() != 0) {
      delete g_dfRotary[rotaryIndex];
      g_dfRotary[rotaryIndex] = nullptr;
      i2c_hub_deselect();
      return false;
    }
    g_dfRotary[rotaryIndex]->setGainCoefficient(cfg::kRotaryGainCoeff);
    g_dfRotary[rotaryIndex]->setEncoderValue(kDfRotaryCenter);
    g_prevDfRotaryValue[rotaryIndex] = kDfRotaryCenter;
    g_dfRotarySmoothed[rotaryIndex]  = static_cast<float>(kDfRotaryCenter);
  }

  i2c_hub_deselect();
  return true;
}

void poll_df_rotary_locked() {
  for (uint8_t rotaryIndex = 0; rotaryIndex < kDfRotaryCount; ++rotaryIndex) {
    bool detectedNow = detect_df_rotary_locked(rotaryIndex);
    if (detectedNow != g_dfRotaryDetected[rotaryIndex]) {
      g_dfRotaryDetected[rotaryIndex] = detectedNow;
      if (cfg::kDebugLog) {
        Serial.printf("[SlavePico][I2C] DFRobot%u %s on hub ch%u\n",
                      rotaryIndex,
                      detectedNow ? "detected" : "missing",
                      static_cast<unsigned>(devices::HUB_CH_DF_ROT_0 + rotaryIndex));
      }
    }
    if (!detectedNow || !g_dfRotary[rotaryIndex]) continue;

    uint8_t hubChannel = static_cast<uint8_t>(devices::HUB_CH_DF_ROT_0 + rotaryIndex);
    if (!i2c_hub_select(hubChannel)) continue;
    uint16_t value = g_dfRotary[rotaryIndex]->getEncoderValue();
    bool buttonDown = g_dfRotary[rotaryIndex]->detectButtonDown();
    i2c_hub_deselect();

    // EMA low-pass filter: smooths noise and rapid jitter
    g_dfRotarySmoothed[rotaryIndex] =
        cfg::kRotarySmoothAlpha * static_cast<float>(value) +
        (1.0f - cfg::kRotarySmoothAlpha) * g_dfRotarySmoothed[rotaryIndex];
    uint16_t smoothed = static_cast<uint16_t>(g_dfRotarySmoothed[rotaryIndex] + 0.5f);

    uint32_t nowMs = millis();
    bool changed  = abs((int)smoothed - (int)g_prevDfRotaryValue[rotaryIndex]) >= kDfRotaryStepThreshold;
    bool throttleOk = (nowMs - g_lastDfRotaryEventMs[rotaryIndex]) >= cfg::kRotaryMinSendIntervalMs;
    if (changed && throttleOk) {
      g_prevDfRotaryValue[rotaryIndex]  = smoothed;
      g_lastDfRotaryEventMs[rotaryIndex] = nowMs;
      (void)push_event(static_cast<uint8_t>(devices::CTRL_DF_ROTARY_0 + rotaryIndex), 0, static_cast<int16_t>(smoothed), 2);
      if (cfg::kDebugLog) {
        Serial.printf("[SlavePico][I2C] DFRobot%u smoothed=%u raw=%u\n", rotaryIndex, smoothed, value);
      }
    }

    uint32_t now = millis();
    if (buttonDown && !g_prevDfRotaryButtonDown[rotaryIndex] && (now - g_lastDfButtonMs[rotaryIndex] >= kDfButtonGuardMs)) {
      g_lastDfButtonMs[rotaryIndex] = now;
      (void)push_event(static_cast<uint8_t>(devices::CTRL_DF_ROTARY_0 + rotaryIndex), 0, 1, 1);
      if (cfg::kDebugLog) {
        Serial.printf("[SlavePico][I2C] DFRobot%u button\n", rotaryIndex);
      }
    }
    g_prevDfRotaryButtonDown[rotaryIndex] = buttonDown;
  }
}

void poll_hub_base_isolation_locked() {
  uint32_t now = millis();
  if (now < g_hubRecoverUntilMs) {
    return;
  }

  bool switchOk = i2c_hub_select(g_hubProbeChannel);
  if (switchOk) {
    g_hubRecoverCount = 0;
    i2c_hub_deselect();
  } else {
    if (g_hubRecoverCount < 255) g_hubRecoverCount++;
    bool reinitOk = i2c_driver_reinit();
    g_hubRecoverUntilMs = now + 250;
    if (cfg::kDebugLog) {
      Serial.printf("[SlavePico][I2C] base recover ch=%u reinit=%d count=%u\n",
                    g_hubProbeChannel,
                    reinitOk ? 1 : 0,
                    g_hubRecoverCount);
    }
  }

  if (cfg::kDebugLog && now - g_lastHubProbeLogMs >= 500) {
    g_lastHubProbeLogMs = now;
    Serial.printf("[SlavePico][I2C] base probe ch=%u switch=%d hub=%d\n",
                  g_hubProbeChannel,
                  switchOk ? 1 : 0,
                  g_hubDetected ? 1 : 0);
  }

  g_hubProbeChannel = static_cast<uint8_t>((g_hubProbeChannel + 1) & 0x07);
}

bool read_bytebutton_status_locked(uint8_t channel, uint8_t& status) {
  status = 0;
  bool ok = false;
  if (!i2c_hub_select(channel)) return false;

  uint8_t statusBytes[kByteButtonButtons] = {};
  Wire.beginTransmission(cfg::kAddrM5ByteButton);
  Wire.write(kByteButtonStatus8ByteReg);
  if (Wire.endTransmission() == 0 &&
      Wire.requestFrom((uint8_t)cfg::kAddrM5ByteButton, (uint8_t)kByteButtonButtons) == kByteButtonButtons) {
    for (uint8_t i = 0; i < kByteButtonButtons; ++i) {
      statusBytes[i] = Wire.read();
      if (statusBytes[i]) status |= (uint8_t)(1u << i);
    }
    ok = true;
  } else {
    Wire.beginTransmission(cfg::kAddrM5ByteButton);
    Wire.write(kByteButtonStatusReg);
    if (Wire.endTransmission() == 0 &&
        Wire.requestFrom((uint8_t)cfg::kAddrM5ByteButton, (uint8_t)1) == 1) {
      status = Wire.read();
      ok = true;
    }
  }

  i2c_hub_deselect();
  return ok;
}

bool bytebutton_write_bytes(uint8_t reg, const uint8_t* data, uint8_t length) {
  Wire.beginTransmission(cfg::kAddrM5ByteButton);
  Wire.write(reg);
  for (uint8_t i = 0; i < length; ++i) {
    Wire.write(data[i]);
  }
  return Wire.endTransmission() == 0;
}

bool init_bytebutton_leds_locked(uint8_t channel) {
  bool ok = false;
  if (!i2c_hub_select(channel)) return false;
  uint8_t mode = kByteButtonLedUserDefined;
  if (bytebutton_write_bytes(kByteButtonLedShowModeReg, &mode, 1)) {
    ok = true;
    for (uint8_t led = 0; led < kByteButtonLedCount; ++led) {
      uint8_t brightness = kByteButtonBrightness;
      if (!bytebutton_write_bytes(kByteButtonLedBrightnessReg + led, &brightness, 1)) {
        ok = false;
        break;
      }
    }
  }
  i2c_hub_deselect();
  return ok;
}

bool write_bytebutton_led_locked(uint8_t channel, uint8_t led, uint8_t red, uint8_t green, uint8_t blue) {
  if (led >= kByteButtonLedCount) return false;
  if (!i2c_hub_select(channel)) return false;
  uint8_t rgb4[4] = {red, green, blue, 0};
  bool ok = bytebutton_write_bytes(kByteButtonLedRgb888Reg + led * 4, rgb4, 4);
  i2c_hub_deselect();
  return ok;
}

void write_bytebutton_idle_locked() {
  // Module 0: B0=PLAY(stopped), B1=PAT-, B2=PAT+, B3=PAT1,
  //           B4=MUTE_ALL, B5=PHASER, B6=DELAY, B7=REVERB
  static const uint8_t kR0[8] = {25,  0,  0, 35, 20, 15,  0,  0};
  static const uint8_t kG0[8] = { 0, 15, 15, 30, 15,  0,  5, 15};
  static const uint8_t kB0[8] = { 0, 40, 40,  0,  0, 15, 20, 15};
  for (uint8_t led = 0; led < kByteButtonLedCount; ++led) {
    if (led < 8) {
      (void)write_bytebutton_led_locked(devices::HUB_CH_BYTEBTN_0, led, kR0[led], kG0[led], kB0[led]);
    } else {
      (void)write_bytebutton_led_locked(devices::HUB_CH_BYTEBTN_0, led, 0, 8, 24);
    }
  }
}

void write_bytebutton1_idle_locked() {
  // Module 1: B0=CLEAR PAT(red), B1=STEPS×16(white), B2=STEPS×32(blue), B3=STEPS×64(cyan),
  //           B4=FLANGER(pink),  B5=CHORUS(orange),  B6=COMPRESSOR(green), B7=SONG MODE(yellow)
  static const uint8_t kR1[8] = {40,  20,   0,  0, 50, 45,  0, 35};
  static const uint8_t kG1[8] = { 0,  20,  12, 30,  0, 20, 50, 30};
  static const uint8_t kB1[8] = { 0,  20,  55, 35, 25,  0,  0,  0};
  for (uint8_t led = 0; led < kByteButtonLedCount; ++led) {
    if (led < 8) {
      (void)write_bytebutton_led_locked(devices::HUB_CH_BYTEBTN_1, led, kR1[led], kG1[led], kB1[led]);
    } else {
      (void)write_bytebutton_led_locked(devices::HUB_CH_BYTEBTN_1, led, 0, 8, 24);
    }
  }
}

void write_m5encoder_idle_locked() {
  if (g_m5Encoder0Detected && i2c_hub_select(devices::HUB_CH_M5_ENC_0)) {
    for (uint8_t enc = 0; enc < kEncodersPerModule; ++enc) {
      uint8_t r = 8, gv = static_cast<uint8_t>(14 + enc * 3), b = static_cast<uint8_t>(26 + enc * 8);
      (void)g_m5Encoder0.writeRGB(enc, r, gv, b);
    }
    i2c_hub_deselect();
  }
  if (g_m5Encoder1Detected && i2c_hub_select(devices::HUB_CH_M5_ENC_1)) {
    for (uint8_t enc = 0; enc < kEncodersPerModule; ++enc) {
      uint8_t r = 8, gv = static_cast<uint8_t>(14 + enc * 3), b = static_cast<uint8_t>(26 + enc * 8);
      (void)g_m5Encoder1.writeRGB(enc, r, gv, b);
    }
    i2c_hub_deselect();
  }
}

void run_led_boot_animation_locked() {
  uint32_t now = millis();
  if (now - g_lastLedAnimMs < 90) return;
  g_lastLedAnimMs = now;

  if (!g_bootAnimDone) {
    for (uint8_t eiMod = 0; eiMod < 2; eiMod++) {
      bool encDet = (eiMod == 0) ? g_m5Encoder0Detected : g_m5Encoder1Detected;
      if (!encDet) continue;
      uint8_t encCh = (eiMod == 0) ? (uint8_t)devices::HUB_CH_M5_ENC_0 : (uint8_t)devices::HUB_CH_M5_ENC_1;
      if (!i2c_hub_select(encCh)) continue;
      for (uint8_t enc = 0; enc < kEncodersPerModule; ++enc) {
        if (enc == g_bootAnimPhase % kEncodersPerModule) {
          if (eiMod == 0) (void)g_m5Encoder0.writeRGB(enc, 90, 10, 0);
          else            (void)g_m5Encoder1.writeRGB(enc, 10, 0, 90);
        } else {
          if (eiMod == 0) (void)g_m5Encoder0.writeRGB(enc, 0, 0, 6);
          else            (void)g_m5Encoder1.writeRGB(enc, 0, 0, 6);
        }
      }
      i2c_hub_deselect();
    }

    for (uint8_t mod = 0; mod < 2; mod++) {
      bool rdy = (mod == 0) ? (g_byteButton0Detected && g_byteButton0LedsReady)
                             : (g_byteButton1Detected && g_byteButton1LedsReady);
      uint8_t ch = (mod == 0) ? devices::HUB_CH_BYTEBTN_0 : devices::HUB_CH_BYTEBTN_1;
      if (rdy) {
        for (uint8_t led = 0; led < kByteButtonLedCount; ++led) {
          uint8_t red = 0, green = 0, blue = 4;
          if (led == g_bootAnimPhase % kByteButtonLedCount) {
            red = (mod == 0) ? 0 : 60; green = (mod == 0) ? 70 : 25; blue = 24;
          }
          (void)write_bytebutton_led_locked(ch, led, red, green, blue);
        }
      }
    }

    g_bootAnimPhase++;
    if (g_bootAnimPhase >= 18) {
      g_bootAnimDone = true;
      if (g_m5Encoder0Detected) write_m5encoder_idle_locked();
      if (g_byteButton0Detected && g_byteButton0LedsReady) write_bytebutton_idle_locked();
      if (g_byteButton1Detected && g_byteButton1LedsReady) write_bytebutton1_idle_locked();
    }
  }
}

bool detect_m5encoder0_locked() {
  bool ok = false;
  if (!i2c_hub_select(devices::HUB_CH_M5_ENC_0)) return false;
  if (!probe_selected_bus_device(cfg::kAddrM5Encoder8)) {
    i2c_hub_deselect();
    return false;
  }

  if (!g_m5Encoder0Detected) {
    if (g_m5Encoder0.begin()) {
      for (uint8_t enc = 0; enc < kEncodersPerModule; ++enc) {
        g_m5Encoder0.resetCounter(enc);
        g_prevM5Counter0[enc] = 0;
        g_m5ButtonArmed0[enc] = true;
      }
      ok = true;
    }
  } else {
    ok = true;
  }
  i2c_hub_deselect();
  return ok;
}

bool detect_m5encoder1_locked() {
  bool ok = false;
  if (!i2c_hub_select(devices::HUB_CH_M5_ENC_1)) return false;
  if (!probe_selected_bus_device(cfg::kAddrM5Encoder8)) {
    i2c_hub_deselect();
    return false;
  }
  if (!g_m5Encoder1Detected) {
    if (g_m5Encoder1.begin()) {
      for (uint8_t enc = 0; enc < kEncodersPerModule; ++enc) {
        g_m5Encoder1.resetCounter(enc);
        g_prevM5Counter1[enc] = 0;
        g_m5ButtonArmed1[enc] = true;
      }
      ok = true;
    }
  } else {
    ok = true;
  }
  i2c_hub_deselect();
  return ok;
}

} // namespace

void input_manager_init() {
#if defined(ARDUINO_ARCH_ESP32)
  g_eventQueue = xQueueCreate(kQueueLen, sizeof(InputEvent));
  if (!g_eventQueue) {
    Serial.println("[SlavePico] ERROR: input event queue allocation failed");
  }
#endif
  if (cfg::kEnableFaderAnalog) {
    pinMode(cfg::kFaderAnalogPin, INPUT);
  }
  for (uint8_t rotaryIndex = 0; rotaryIndex < kDfRotaryCount; ++rotaryIndex) {
    g_prevDfRotaryValue[rotaryIndex] = kDfRotaryCenter;
    g_dfRotarySmoothed[rotaryIndex]  = static_cast<float>(kDfRotaryCenter);
  }
  for (uint8_t enc = 0; enc < kEncodersPerModule; ++enc) {
    g_m5ButtonArmed0[enc] = true;
    g_m5ButtonArmed1[enc] = true;
  }

#if defined(ARDUINO_ARCH_ESP32)
  // Binary semaphore used to wake the I2C task.
  // Given by ISR (INT pin) or by timeout fallback (kInputPollMs).
  s_i2c_sem = xSemaphoreCreateBinary();

  if (cfg::kEnableDfRotaryInterrupt) {
    for (int i = 0; i < 4; i++) {
      int8_t pin = cfg::kDfRotaryIntPins[i];
      if (pin < 0) continue;
      pinMode(pin, INPUT_PULLUP);
      attachInterruptArg(
        static_cast<uint8_t>(pin),
        [](void* /*arg*/) IRAM_ATTR {
          BaseType_t woken = pdFALSE;
          xSemaphoreGiveFromISR(s_i2c_sem, &woken);
          if (woken) portYIELD_FROM_ISR();
        },
        nullptr,
        FALLING
      );
    }
  }
#endif
}

void input_manager_poll_i2c() {
  if (!i2c_driver_is_ready()) return;
  if (!i2c_lock(30)) return;

  uint32_t nowMs = millis();
  if (nowMs < g_nextHubProbeMs) {
    i2c_unlock();
    return;
  }
  bool hubDetectedNow = detect_hub_locked();
  if (hubDetectedNow != g_hubDetected) {
    g_hubDetected = hubDetectedNow;
    if (cfg::kDebugLog) {
      Serial.printf("[SlavePico][I2C] hub %s at 0x%02X\n", g_hubDetected ? "detected" : "missing", cfg::kI2cHubAddr);
    }
    (void)push_event(devices::CTRL_I2C_HUB, 0, g_hubDetected ? 1 : 0, 1);
  }

  if (!hubDetectedNow) {
    // Avoid flooding Serial/the bus when the hub is unplugged or unpowered.
    g_nextHubProbeMs = nowMs + 500;
    i2c_unlock();
    return;
  }
  g_nextHubProbeMs = nowMs;

  if (cfg::kI2cBaseIsolationMode) {
    poll_hub_base_isolation_locked();
    i2c_unlock();
    return;
  }

  if (cfg::kEnableDfRotary) {
    poll_df_rotary_locked();
  }

  if (cfg::kEnableM5ByteButton) {
    uint8_t byteButtonStatus = 0;
    bool byteButton0AckNow = false;
    bool byteButton0DetectedNow = false;
    uint32_t nowMs = millis();
    if (i2c_hub_select(devices::HUB_CH_BYTEBTN_0)) {
      byteButton0AckNow = probe_selected_bus_device(cfg::kAddrM5ByteButton);
      i2c_hub_deselect();
    }
    if (byteButton0AckNow) {
      if (g_byteButton0PresenceCount < 255) g_byteButton0PresenceCount++;
    } else {
      g_byteButton0PresenceCount = 0;
    }

    if (nowMs >= g_byteButton0SuspendUntilMs && g_byteButton0PresenceCount >= 2) {
      byteButton0DetectedNow = read_bytebutton_status_locked(devices::HUB_CH_BYTEBTN_0, byteButtonStatus);
      if (byteButton0DetectedNow) {
        g_byteButton0FailCount = 0;
      } else {
        if (g_byteButton0FailCount < 255) g_byteButton0FailCount++;
        if (g_byteButton0FailCount >= 3) {
          g_byteButton0SuspendUntilMs = nowMs + 750;
          if (cfg::kDebugLog) {
            Serial.println("[SlavePico][I2C] ByteButton0 suspended after repeated read errors");
          }
        }
      }
    }

    if (byteButton0DetectedNow != g_byteButton0Detected) {
      g_byteButton0Detected = byteButton0DetectedNow;
      if (g_byteButton0Detected) {
        g_byteButton0LedsReady = init_bytebutton_leds_locked(devices::HUB_CH_BYTEBTN_0);
        if (!g_bootAnimDone && g_byteButton0LedsReady) {
          write_bytebutton_idle_locked();
        }
      } else {
        g_byteButton0LedsReady = false;
      }
      if (cfg::kDebugLog) {
        Serial.printf("[SlavePico][I2C] ByteButton0 %s on hub ch%d\n", g_byteButton0Detected ? "detected" : "missing", devices::HUB_CH_BYTEBTN_0);
      }
    }

    if (byteButton0DetectedNow) {
      uint8_t pressedEdges = byteButtonStatus & (uint8_t)~g_prevByteButton0State;
      g_prevByteButton0State = byteButtonStatus;
      for (uint8_t button = 0; button < kByteButtonButtons; ++button) {
        if ((pressedEdges & (1u << button)) == 0) continue;
        (void)push_event(devices::CTRL_BYTEBTN_0, button, 1, 1);
        // Brief white flash on press; udp_handler will set the final state color
        if (g_byteButton0LedsReady) {
          (void)write_bytebutton_led_locked(devices::HUB_CH_BYTEBTN_0, button, 80, 80, 80);
        }
        if (cfg::kDebugLog) {
          Serial.printf("[SlavePico][I2C] ByteButton0 press b=%u mask=0x%02X\n", button, byteButtonStatus);
        }
      }
      // Apply pending LED state colors queued by udp_handler (settled from previous press)
      if (g_byteButton0LedsReady) {
        for (uint8_t i = 0; i < kByteButtonButtons; i++) {
          LedRequest request{};
          if (!take_pending_led(g_pendingLed0, i, request)) continue;
          (void)write_bytebutton_led_locked(devices::HUB_CH_BYTEBTN_0, i,
            request.r, request.g, request.b);
        }
      }
    }

    // ── ByteButton module 1 (hub ch 1) ──────────────────────────────────────
    uint8_t byteButton1Status = 0;
    bool byteButton1AckNow = false;
    bool byteButton1DetectedNow = false;
    uint32_t nowMs1 = millis();
    if (i2c_hub_select(devices::HUB_CH_BYTEBTN_1)) {
      byteButton1AckNow = probe_selected_bus_device(cfg::kAddrM5ByteButton);
      i2c_hub_deselect();
    }
    if (byteButton1AckNow) {
      if (g_byteButton1PresenceCount < 255) g_byteButton1PresenceCount++;
    } else {
      g_byteButton1PresenceCount = 0;
    }
    if (nowMs1 >= g_byteButton1SuspendUntilMs && g_byteButton1PresenceCount >= 2) {
      byteButton1DetectedNow = read_bytebutton_status_locked(devices::HUB_CH_BYTEBTN_1, byteButton1Status);
      if (byteButton1DetectedNow) {
        g_byteButton1FailCount = 0;
      } else {
        if (g_byteButton1FailCount < 255) g_byteButton1FailCount++;
        if (g_byteButton1FailCount >= 3) {
          g_byteButton1SuspendUntilMs = nowMs1 + 750;
        }
      }
    }
    if (byteButton1DetectedNow != g_byteButton1Detected) {
      g_byteButton1Detected = byteButton1DetectedNow;
      if (g_byteButton1Detected) {
        g_byteButton1LedsReady = init_bytebutton_leds_locked(devices::HUB_CH_BYTEBTN_1);
        if (g_byteButton1LedsReady) write_bytebutton1_idle_locked();
      } else {
        g_byteButton1LedsReady = false;
      }
      if (cfg::kDebugLog) {
        Serial.printf("[SlavePico][I2C] ByteButton1 %s on hub ch%d\n",
          g_byteButton1Detected ? "detected" : "missing", devices::HUB_CH_BYTEBTN_1);
      }
    }
    if (byteButton1DetectedNow) {
      uint8_t pressedEdges1 = byteButton1Status & (uint8_t)~g_prevByteButton1State;
      g_prevByteButton1State = byteButton1Status;
      for (uint8_t button = 0; button < kByteButtonButtons; ++button) {
        if ((pressedEdges1 & (1u << button)) == 0) continue;
        (void)push_event(devices::CTRL_BYTEBTN_1, button, 1, 1);
        if (g_byteButton1LedsReady) {
          (void)write_bytebutton_led_locked(devices::HUB_CH_BYTEBTN_1, button, 80, 80, 80);
        }
        if (cfg::kDebugLog) {
          Serial.printf("[SlavePico][I2C] ByteButton1 press b=%u mask=0x%02X\n", button, byteButton1Status);
        }
      }
      if (g_byteButton1LedsReady) {
        for (uint8_t i = 0; i < kByteButtonButtons; i++) {
          LedRequest request{};
          if (!take_pending_led(g_pendingLed1, i, request)) continue;
          (void)write_bytebutton_led_locked(devices::HUB_CH_BYTEBTN_1, i,
            request.r, request.g, request.b);
        }
      }
    }
  }

  if (cfg::kEnableM5Encoder) {
    // ── M5Encoder module 0 (hub ch 2, tracks 0-7) ──────────────────────────
    bool m5Encoder0AckNow = false;
    if (i2c_hub_select(devices::HUB_CH_M5_ENC_0)) {
      m5Encoder0AckNow = probe_selected_bus_device(cfg::kAddrM5Encoder8);
      i2c_hub_deselect();
    }
    if (m5Encoder0AckNow) {
      if (g_m5Encoder0PresenceCount < 255) g_m5Encoder0PresenceCount++;
    } else {
      g_m5Encoder0PresenceCount = 0;
    }
    bool m5Encoder0DetectedNow = false;
    if (g_m5Encoder0PresenceCount >= 2) {
      m5Encoder0DetectedNow = detect_m5encoder0_locked();
    }
    if (m5Encoder0DetectedNow != g_m5Encoder0Detected) {
      g_m5Encoder0Detected = m5Encoder0DetectedNow;
      if (g_m5Encoder0Detected) write_m5encoder_idle_locked();
      if (cfg::kDebugLog) {
        Serial.printf("[SlavePico][I2C] M5Encoder0 %s on hub ch%d\n", g_m5Encoder0Detected ? "detected" : "missing", devices::HUB_CH_M5_ENC_0);
      }
    }
    if (m5Encoder0DetectedNow && i2c_hub_select(devices::HUB_CH_M5_ENC_0)) {
      for (uint8_t enc = 0; enc < kEncodersPerModule; ++enc) {
        int32_t counter = g_m5Encoder0.getAbsCounter(enc);
        int32_t delta = counter - g_prevM5Counter0[enc];
        if (delta != 0) {
          g_prevM5Counter0[enc] = counter;
          if (abs((int)delta) <= kM5DeltaClamp) {
            int16_t signedDelta = static_cast<int16_t>(-delta);
            (void)push_event(devices::CTRL_M5_ENC_BANK_0, enc, signedDelta, 0);
            if (cfg::kDebugLog) Serial.printf("[SlavePico][I2C] M5Enc0 enc=%u d=%d\n", enc, signedDelta);
          }
        }
        bool btnNow = g_m5Encoder0.getKeyPressed(enc);
        if (btnNow) {
          if (g_m5ButtonConfirm0[enc] < 255) g_m5ButtonConfirm0[enc]++;
        } else {
          g_m5ButtonConfirm0[enc] = 0;
          g_m5ButtonArmed0[enc] = true;
        }
        if (g_m5ButtonConfirm0[enc] >= 3 && g_m5ButtonArmed0[enc]) {
          unsigned long now = millis();
          if (now - g_lastM5ButtonPress0[enc] >= 250) {
            g_lastM5ButtonPress0[enc] = now;
            g_m5ButtonArmed0[enc] = false;
            (void)push_event(devices::CTRL_M5_ENC_BANK_0, enc, 1, 1);
            if (cfg::kDebugLog) Serial.printf("[SlavePico][I2C] M5Enc0 btn enc=%u\n", enc);
          }
        }
      }
      for (uint8_t enc = 0; enc < kEncodersPerModule; ++enc) {
        EncLedRequest request{};
        if (!take_pending_led(g_pendingEncLed0, enc, request)) continue;
        (void)g_m5Encoder0.writeRGB(enc, request.r, request.g, request.b);
      }
      i2c_hub_deselect();
    }

    // ── M5Encoder module 1 (hub ch 3, tracks 8-15) ─────────────────────────
    bool m5Encoder1AckNow = false;
    if (i2c_hub_select(devices::HUB_CH_M5_ENC_1)) {
      m5Encoder1AckNow = probe_selected_bus_device(cfg::kAddrM5Encoder8);
      i2c_hub_deselect();
    }
    if (m5Encoder1AckNow) {
      if (g_m5Encoder1PresenceCount < 255) g_m5Encoder1PresenceCount++;
    } else {
      g_m5Encoder1PresenceCount = 0;
    }
    bool m5Encoder1DetectedNow = false;
    if (g_m5Encoder1PresenceCount >= 2) {
      m5Encoder1DetectedNow = detect_m5encoder1_locked();
    }
    if (m5Encoder1DetectedNow != g_m5Encoder1Detected) {
      g_m5Encoder1Detected = m5Encoder1DetectedNow;
      if (g_m5Encoder1Detected) write_m5encoder_idle_locked();
      if (cfg::kDebugLog) {
        Serial.printf("[SlavePico][I2C] M5Encoder1 %s on hub ch%d\n", g_m5Encoder1Detected ? "detected" : "missing", devices::HUB_CH_M5_ENC_1);
      }
    }
    if (m5Encoder1DetectedNow && i2c_hub_select(devices::HUB_CH_M5_ENC_1)) {
      for (uint8_t enc = 0; enc < kEncodersPerModule; ++enc) {
        int32_t counter = g_m5Encoder1.getAbsCounter(enc);
        int32_t delta = counter - g_prevM5Counter1[enc];
        if (delta != 0) {
          g_prevM5Counter1[enc] = counter;
          if (abs((int)delta) <= kM5DeltaClamp) {
            int16_t signedDelta = static_cast<int16_t>(-delta);
            (void)push_event(devices::CTRL_M5_ENC_BANK_1, enc, signedDelta, 0);
            if (cfg::kDebugLog) Serial.printf("[SlavePico][I2C] M5Enc1 enc=%u d=%d\n", enc, signedDelta);
          }
        }
        bool btnNow = g_m5Encoder1.getKeyPressed(enc);
        if (btnNow) {
          if (g_m5ButtonConfirm1[enc] < 255) g_m5ButtonConfirm1[enc]++;
        } else {
          g_m5ButtonConfirm1[enc] = 0;
          g_m5ButtonArmed1[enc] = true;
        }
        if (g_m5ButtonConfirm1[enc] >= 3 && g_m5ButtonArmed1[enc]) {
          unsigned long now = millis();
          if (now - g_lastM5ButtonPress1[enc] >= 250) {
            g_lastM5ButtonPress1[enc] = now;
            g_m5ButtonArmed1[enc] = false;
            (void)push_event(devices::CTRL_M5_ENC_BANK_1, enc, 1, 1);
            if (cfg::kDebugLog) Serial.printf("[SlavePico][I2C] M5Enc1 btn enc=%u\n", enc);
          }
        }
      }
      for (uint8_t enc = 0; enc < kEncodersPerModule; ++enc) {
        EncLedRequest request{};
        if (!take_pending_led(g_pendingEncLed1, enc, request)) continue;
        (void)g_m5Encoder1.writeRGB(enc, request.r, request.g, request.b);
      }
      i2c_hub_deselect();
    }
  }

  if (cfg::kEnableM5ByteButton || cfg::kEnableM5Encoder) {
    run_led_boot_animation_locked();
  }

  i2c_hub_deselect();
  i2c_unlock();
}

void input_manager_poll_analog() {
  if (!cfg::kEnableFaderAnalog) return;
  int raw = analogRead(cfg::kFaderAnalogPin);
  if (g_lastFader < 0) {
    g_lastFader = static_cast<int16_t>(raw);
    return;
  }
  if (abs(raw - g_lastFader) >= 8) {
    g_lastFader = static_cast<int16_t>(raw);
    (void)push_event(devices::CTRL_FADER_0, 0, static_cast<int16_t>(raw), 2);
  }
}

bool input_manager_pop_event(InputEvent& out) {
#if defined(ARDUINO_ARCH_ESP32)
  return g_eventQueue && xQueueReceive(g_eventQueue, &out, 0) == pdTRUE;
#else
  if (g_tail == g_head) return false;
  out = g_queue[g_tail];
  g_tail = static_cast<uint8_t>((g_tail + 1) % kQueueLen);
  return true;
#endif
}

uint32_t input_manager_dropped_event_count() {
  return g_droppedEvents;
}

#if defined(ARDUINO_ARCH_ESP32)
SemaphoreHandle_t input_get_i2c_semaphore() {
  return s_i2c_sem;
}
#endif

void input_set_bytebutton_led(uint8_t btn, uint8_t r, uint8_t g, uint8_t b) {
  if (btn >= kByteButtonButtons) return;
#if defined(ARDUINO_ARCH_ESP32)
  portENTER_CRITICAL(&g_ledMux);
#endif
  g_pendingLed0[btn] = {r, g, b, true};
#if defined(ARDUINO_ARCH_ESP32)
  portEXIT_CRITICAL(&g_ledMux);
#endif
}

void input_set_bytebutton1_led(uint8_t btn, uint8_t r, uint8_t g, uint8_t b) {
  if (btn >= kByteButtonButtons) return;
#if defined(ARDUINO_ARCH_ESP32)
  portENTER_CRITICAL(&g_ledMux);
#endif
  g_pendingLed1[btn] = {r, g, b, true};
#if defined(ARDUINO_ARCH_ESP32)
  portEXIT_CRITICAL(&g_ledMux);
#endif
}

void input_set_enc0_led(uint8_t enc, uint8_t r, uint8_t g, uint8_t b) {
  if (enc >= kEncodersPerModule) return;
#if defined(ARDUINO_ARCH_ESP32)
  portENTER_CRITICAL(&g_ledMux);
#endif
  g_pendingEncLed0[enc] = {r, g, b, true};
#if defined(ARDUINO_ARCH_ESP32)
  portEXIT_CRITICAL(&g_ledMux);
#endif
}

void input_set_enc1_led(uint8_t enc, uint8_t r, uint8_t g, uint8_t b) {
  if (enc >= kEncodersPerModule) return;
#if defined(ARDUINO_ARCH_ESP32)
  portENTER_CRITICAL(&g_ledMux);
#endif
  g_pendingEncLed1[enc] = {r, g, b, true};
#if defined(ARDUINO_ARCH_ESP32)
  portEXIT_CRITICAL(&g_ledMux);
#endif
}
