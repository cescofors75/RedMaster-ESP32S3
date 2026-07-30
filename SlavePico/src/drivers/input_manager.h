#pragma once

#include <stdint.h>
#if defined(ARDUINO_ARCH_ESP32)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

struct InputEvent {
  uint8_t controlId;
  uint8_t elementId;
  int16_t value;
  uint8_t eventType; // 0=delta, 1=button, 2=absolute
};

void input_manager_init();
void input_manager_poll_i2c();
void input_manager_poll_analog();
bool input_manager_pop_event(InputEvent& out);
uint32_t input_manager_dropped_event_count();

// Set a ByteButton LED color from any task (module 0 or 1).
// The I2C task picks it up on the next poll cycle.
void input_set_bytebutton_led(uint8_t btn, uint8_t r, uint8_t g, uint8_t b);   // module 0
void input_set_bytebutton1_led(uint8_t btn, uint8_t r, uint8_t g, uint8_t b);  // module 1
void input_set_enc0_led(uint8_t enc, uint8_t r, uint8_t g, uint8_t b);         // M5Encoder module 0 (tracks 0-7)
void input_set_enc1_led(uint8_t enc, uint8_t r, uint8_t g, uint8_t b);         // M5Encoder module 1 (tracks 8-15)

#if defined(ARDUINO_ARCH_ESP32)
// Returns the semaphore the I2C task should block on.
// ISRs from INT pins (and other wake sources) give this semaphore.
SemaphoreHandle_t input_get_i2c_semaphore();
#endif
