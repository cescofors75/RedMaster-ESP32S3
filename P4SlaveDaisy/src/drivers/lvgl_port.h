#pragma once

#include <lvgl.h>

// Zero-copy, double-buffered LVGL port for the JC1060P470C. The render task
// owns lv_timer_handler(); external UI mutations must hold this lock.
bool lvgl_port_init();
bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock();
void lvgl_port_start();
uint32_t lvgl_port_last_touch_ms();

