#pragma once

#include <Arduino.h>

#ifndef P4_ENABLE_DEBUG_LOG
#define P4_ENABLE_DEBUG_LOG 1
#endif

#if P4_ENABLE_DEBUG_LOG
#define P4_LOG_PRINT(...)   Serial.print(__VA_ARGS__)
#define P4_LOG_PRINTLN(...) Serial.println(__VA_ARGS__)
#define P4_LOG_PRINTF(...)  Serial.printf(__VA_ARGS__)
#else
#define P4_LOG_PRINT(...)   ((void)0)
#define P4_LOG_PRINTLN(...) ((void)0)
#define P4_LOG_PRINTF(...)  ((void)0)
#endif

#ifndef LCD_H_RES
#define LCD_H_RES 1024
#endif
#ifndef LCD_V_RES
#define LCD_V_RES 600
#endif

#define UI_W LCD_H_RES
#define UI_H LCD_V_RES

#ifndef MIPI_DSI_LANES
#define MIPI_DSI_LANES 2
#endif
#ifndef MIPI_DSI_LANE_BITRATE_MBPS
#define MIPI_DSI_LANE_BITRATE_MBPS 550
#endif

#define LCD_HSYNC_PULSE 24
#define LCD_HSYNC_BACK  136
#define LCD_HSYNC_FRONT 160
#define LCD_VSYNC_PULSE 2
#define LCD_VSYNC_BACK  21
#define LCD_VSYNC_FRONT 12

#ifndef LCD_BL_GPIO
#define LCD_BL_GPIO 23
#endif
#ifndef LCD_RST_GPIO
#define LCD_RST_GPIO 27
#endif

#ifndef TOUCH_I2C_SDA
#define TOUCH_I2C_SDA 7
#endif
#ifndef TOUCH_I2C_SCL
#define TOUCH_I2C_SCL 8
#endif
#ifndef TOUCH_I2C_ADDR
#define TOUCH_I2C_ADDR 0x5D
#endif
#ifndef TOUCH_RST_GPIO
#define TOUCH_RST_GPIO 22
#endif
#ifndef TOUCH_INT_GPIO
#define TOUCH_INT_GPIO 21
#endif

constexpr uint16_t kDaisyUsbVid = 0x0483;
constexpr uint16_t kDaisyUsbPid = 0x5740;
constexpr uint32_t kTelemetryStaleMs = 750;

