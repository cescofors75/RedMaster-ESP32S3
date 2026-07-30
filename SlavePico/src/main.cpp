#include <Arduino.h>
#include <esp_log.h>
#include <WiFi.h>

#include "config.h"
#include "drivers/i2c_driver.h"
#include "drivers/input_manager.h"
#include "comm/udp_handler.h"

#if defined(ARDUINO_ARCH_ESP32)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace {

uint32_t g_lastSerialAliveMs = 0;

void task_input_i2c(void* /*arg*/) {
  SemaphoreHandle_t sem = input_get_i2c_semaphore();
  for (;;) {
    // Block until INT fires (instant) or kInputPollMs timeout (fallback).
    // When kEnableDfRotaryInterrupt=false, only the timeout path runs.
    if (sem) {
      xSemaphoreTake(sem, pdMS_TO_TICKS(cfg::kInputPollMs));
    } else {
      vTaskDelay(pdMS_TO_TICKS(cfg::kInputPollMs));
    }
    input_manager_poll_i2c();
  }
}

void task_input_analog(void* /*arg*/) {
  for (;;) {
    input_manager_poll_analog();
    vTaskDelay(pdMS_TO_TICKS(cfg::kFaderPollMs));
  }
}

void drain_events_to_udp() {
  InputEvent ev{};
  while (input_manager_pop_event(ev)) {
    udp_send_event(ev);
  }
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println("[SlavePico] boot");

  // Silence framework-side hosted/rpc noise on P4 while using local WiFi STA.
  esp_log_level_set("rpc_core", ESP_LOG_NONE);
  esp_log_level_set("esp32-hal-hosted", ESP_LOG_NONE);

  if (!i2c_driver_init()) {
    Serial.println("[SlavePico] ERROR: i2c init failed");
  }

  input_manager_init();
  udp_handler_init();

#if defined(ARDUINO_ARCH_ESP32)
  if (cfg::kEnableI2cPolling) {
    if (xTaskCreate(task_input_i2c, "input_i2c", 4096, nullptr, 2, nullptr) != pdPASS) {
      Serial.println("[SlavePico] ERROR: input_i2c task creation failed");
    }
  }
  if (xTaskCreate(task_input_analog, "input_analog", 3072, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("[SlavePico] ERROR: input_analog task creation failed");
  }
#endif
}

void loop() {
  udp_handler_process();
  drain_events_to_udp();
  if (cfg::kDebugLog) {
    uint32_t now = millis();
    if (now - g_lastSerialAliveMs >= 3000) {
      g_lastSerialAliveMs = now;
      uint32_t dropped = input_manager_dropped_event_count();
      Serial.printf("[SlavePico] alive wifi=%d udp=%d dropped=%lu\n", static_cast<int>(WiFi.status()), udp_is_ready() ? 1 : 0, static_cast<unsigned long>(dropped));
    }
  }
  delay(2);
}
