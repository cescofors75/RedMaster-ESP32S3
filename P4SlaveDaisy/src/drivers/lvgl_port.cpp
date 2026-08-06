#include "lvgl_port.h"

#include "config.h"
#include "display_init.h"

#include <Arduino.h>
#include <Wire.h>
#include <atomic>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace
{
SemaphoreHandle_t lvgl_mutex_    = nullptr;
SemaphoreHandle_t vsync_done_    = nullptr;
SemaphoreHandle_t swap_pending_  = nullptr;
volatile bool     render_enabled_ = false;
std::atomic<uint32_t> refresh_sequence_{0};

lv_disp_draw_buf_t draw_buffer_;
lv_disp_drv_t      display_driver_;
lv_indev_drv_t     touch_driver_;

struct TouchState
{
    lv_point_t       point;
    lv_indev_state_t state;
};

TouchState touch_ = {{0, 0}, LV_INDEV_STATE_REL};
portMUX_TYPE touch_mux_ = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t last_touch_ms_ = 0;
bool gt911_ready_ = false;

bool IRAM_ATTR OnRefreshDone(esp_lcd_panel_handle_t,
                             esp_lcd_dpi_panel_event_data_t*,
                             void*)
{
    BaseType_t woken = pdFALSE;
    refresh_sequence_.fetch_add(1, std::memory_order_relaxed);
    if(swap_pending_
       && xSemaphoreTakeFromISR(swap_pending_, &woken) == pdTRUE)
        xSemaphoreGiveFromISR(vsync_done_, &woken);
    return woken == pdTRUE;
}

void Flush(lv_disp_drv_t* driver, const lv_area_t*, lv_color_t* pixels)
{
    if(!lv_disp_flush_is_last(driver))
    {
        lv_disp_flush_ready(driver);
        return;
    }

    esp_lcd_panel_draw_bitmap(display_get_panel(), 0, 0,
                              LCD_H_RES, LCD_V_RES, pixels);

    while(xSemaphoreTake(vsync_done_, 0) == pdTRUE) {}
    while(xSemaphoreTake(swap_pending_, 0) == pdTRUE) {}
    const uint32_t before = refresh_sequence_.load(std::memory_order_acquire);
    xSemaphoreGive(swap_pending_);

    const TickType_t started = xTaskGetTickCount();
    const TickType_t limit   = pdMS_TO_TICKS(250);
    bool refreshed = false;
    do
    {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        const TickType_t remain  = elapsed < limit ? limit - elapsed : 0;
        if(xSemaphoreTake(vsync_done_, remain) != pdTRUE)
            break;
        refreshed = refresh_sequence_.load(std::memory_order_acquire) != before;
    } while(!refreshed);

    if(!refreshed)
    {
        xSemaphoreTake(swap_pending_, 0);
        P4_LOG_PRINTLN("[LVGL] VSYNC timeout");
    }
    lv_disp_flush_ready(driver);
}

void PublishTouch(const TouchState& state)
{
    portENTER_CRITICAL(&touch_mux_);
    touch_ = state;
    portEXIT_CRITICAL(&touch_mux_);
}

void ReleaseTouch()
{
    TouchState state = {{0, 0}, LV_INDEV_STATE_REL};
    PublishTouch(state);
}

void InitTouch()
{
    Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL, 400000);
    pinMode(TOUCH_INT_GPIO, OUTPUT);
    pinMode(TOUCH_RST_GPIO, OUTPUT);
    digitalWrite(TOUCH_INT_GPIO, LOW);
    digitalWrite(TOUCH_RST_GPIO, LOW);
    delay(10);
    digitalWrite(TOUCH_RST_GPIO, HIGH);
    delay(50);
    pinMode(TOUCH_INT_GPIO, INPUT);
    delay(50);

    Wire.beginTransmission(TOUCH_I2C_ADDR);
    gt911_ready_ = Wire.endTransmission() == 0;
    P4_LOG_PRINTLN(gt911_ready_ ? "[Touch] GT911 ready"
                                : "[Touch] GT911 not detected");
}

void AcknowledgeTouchFrame()
{
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write(0x81);
    Wire.write(0x4E);
    Wire.write(static_cast<uint8_t>(0));
    Wire.endTransmission();
}

void PollTouch()
{
    if(!gt911_ready_)
    {
        ReleaseTouch();
        return;
    }

    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write(0x81);
    Wire.write(0x4E);
    if(Wire.endTransmission(false) != 0
       || Wire.requestFrom(static_cast<uint8_t>(TOUCH_I2C_ADDR),
                           static_cast<uint8_t>(1)) != 1)
        return;

    const uint8_t status  = Wire.read();
    const uint8_t touches = status & 0x0fu;
    if((status & 0x80u) == 0u)
    {
        if(last_touch_ms_ != 0 && millis() - last_touch_ms_ > 500u)
            ReleaseTouch();
        return;
    }

    if(touches == 0 || touches > 5)
    {
        ReleaseTouch();
        AcknowledgeTouchFrame();
        return;
    }

    uint8_t record[8] = {};
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write(0x81);
    Wire.write(0x4F);
    const bool addressed = Wire.endTransmission(false) == 0;
    const bool received
        = addressed
          && Wire.requestFrom(static_cast<uint8_t>(TOUCH_I2C_ADDR),
                              static_cast<uint8_t>(sizeof(record)))
                 == sizeof(record);
    if(received)
    {
        for(uint8_t& byte : record)
            byte = Wire.read();
        const uint16_t x = static_cast<uint16_t>(record[1])
                           | (static_cast<uint16_t>(record[2]) << 8);
        const uint16_t y = static_cast<uint16_t>(record[3])
                           | (static_cast<uint16_t>(record[4]) << 8);
        TouchState state;
        state.point.x = static_cast<lv_coord_t>(x < LCD_H_RES ? x : LCD_H_RES - 1);
        state.point.y = static_cast<lv_coord_t>(y < LCD_V_RES ? y : LCD_V_RES - 1);
        state.state   = LV_INDEV_STATE_PR;
        PublishTouch(state);
        last_touch_ms_ = millis();
    }
    AcknowledgeTouchFrame();
}

void TouchTask(void*)
{
    while(true)
    {
        PollTouch();
        vTaskDelay(pdMS_TO_TICKS(touch_.state == LV_INDEV_STATE_PR ? 5 : 20));
    }
}

void ReadTouch(lv_indev_drv_t*, lv_indev_data_t* data)
{
    portENTER_CRITICAL(&touch_mux_);
    const TouchState snapshot = touch_;
    portEXIT_CRITICAL(&touch_mux_);
    data->point = snapshot.point;
    data->state = snapshot.state;
}

void RenderTask(void*)
{
    while(!render_enabled_)
        vTaskDelay(pdMS_TO_TICKS(5));

    TickType_t last_wake = xTaskGetTickCount();
    while(true)
    {
        if(lvgl_port_lock(8))
        {
            lv_timer_handler();
            lvgl_port_unlock();
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(16));
    }
}
} // namespace

bool lvgl_port_init()
{
    lvgl_mutex_   = xSemaphoreCreateMutex();
    vsync_done_   = xSemaphoreCreateBinary();
    swap_pending_ = xSemaphoreCreateBinary();
    if(!lvgl_mutex_ || !vsync_done_ || !swap_pending_)
        return false;

    lv_init();

    esp_lcd_dpi_panel_event_callbacks_t callbacks = {};
    callbacks.on_refresh_done = OnRefreshDone;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(
        display_get_panel(), &callbacks, nullptr));

    void* frame0 = nullptr;
    void* frame1 = nullptr;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(
        display_get_panel(), 2, &frame0, &frame1));
    lv_disp_draw_buf_init(&draw_buffer_, frame0, frame1,
                          LCD_H_RES * LCD_V_RES);

    lv_disp_drv_init(&display_driver_);
    display_driver_.hor_res      = LCD_H_RES;
    display_driver_.ver_res      = LCD_V_RES;
    display_driver_.flush_cb     = Flush;
    display_driver_.draw_buf     = &draw_buffer_;
    display_driver_.direct_mode  = 1;
    display_driver_.full_refresh = 0;
    lv_disp_drv_register(&display_driver_);

    InitTouch();
    lv_indev_drv_init(&touch_driver_);
    touch_driver_.type    = LV_INDEV_TYPE_POINTER;
    touch_driver_.read_cb = ReadTouch;
    lv_indev_drv_register(&touch_driver_);

    if(xTaskCreatePinnedToCore(TouchTask, "touch", 4096, nullptr, 6,
                               nullptr, 0) != pdPASS)
        return false;
    if(xTaskCreatePinnedToCore(RenderTask, "lvgl", 12288, nullptr, 5,
                               nullptr, 0) != pdPASS)
        return false;

    P4_LOG_PRINTLN("[LVGL] zero-copy display + GT911 ready");
    return true;
}

bool lvgl_port_lock(uint32_t timeout_ms)
{
    return lvgl_mutex_
           && xSemaphoreTake(lvgl_mutex_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void lvgl_port_unlock()
{
    if(lvgl_mutex_)
        xSemaphoreGive(lvgl_mutex_);
}

void lvgl_port_start()
{
    render_enabled_ = true;
}

uint32_t lvgl_port_last_touch_ms()
{
    return last_touch_ms_;
}
