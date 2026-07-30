#include "drivers/i2c_driver.h"

#include <Wire.h>
#include "config.h"

#if defined(ARDUINO_ARCH_ESP32)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

namespace {
#if defined(ARDUINO_ARCH_ESP32)
SemaphoreHandle_t g_i2cMutex = nullptr;
#endif
bool g_i2cReady = false;
}

bool i2c_driver_init() {
  g_i2cReady = Wire.begin(cfg::kI2cSdaPin, cfg::kI2cSclPin, cfg::kI2cClockHz);
  if (g_i2cReady) {
    Wire.setTimeOut(cfg::kI2cTimeoutMs);
  }
#if defined(ARDUINO_ARCH_ESP32)
  if (!g_i2cMutex) {
    g_i2cMutex = xSemaphoreCreateMutex();
  }
  return g_i2cReady && g_i2cMutex != nullptr;
#else
  return g_i2cReady;
#endif
}

bool i2c_driver_reinit() {
  g_i2cReady = false;
  Wire.end();
  delay(2);
  return i2c_driver_init();
}

bool i2c_driver_is_ready() {
  return g_i2cReady;
}

bool i2c_lock(uint32_t timeoutMs) {
  if (!g_i2cReady) return false;
#if defined(ARDUINO_ARCH_ESP32)
  if (!g_i2cMutex) return false;
  return xSemaphoreTake(g_i2cMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
#else
  (void)timeoutMs;
  return true;
#endif
}

void i2c_unlock() {
#if defined(ARDUINO_ARCH_ESP32)
  if (g_i2cMutex) xSemaphoreGive(g_i2cMutex);
#endif
}

bool i2c_hub_select(uint8_t channel) {
  if (!g_i2cReady) return false;
  if (channel > 7) return false;
  Wire.beginTransmission(cfg::kI2cHubAddr);
  Wire.write(static_cast<uint8_t>(1u << channel));
  bool ok = Wire.endTransmission() == 0;
  if (ok && cfg::kI2cHubSettleUs > 0) {
    delayMicroseconds(cfg::kI2cHubSettleUs);
  }
  return ok;
}

void i2c_hub_deselect() {
  if (!g_i2cReady) return;
  Wire.beginTransmission(cfg::kI2cHubAddr);
  Wire.write(static_cast<uint8_t>(0));
  (void)Wire.endTransmission();
  if (cfg::kI2cHubSettleUs > 0) {
    delayMicroseconds(cfg::kI2cHubSettleUs);
  }
}
