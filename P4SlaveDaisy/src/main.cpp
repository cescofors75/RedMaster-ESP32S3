#include <Arduino.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "drivers/display_init.h"
#include "drivers/lvgl_port.h"
#include "raydrone_link.h"
#include "ui/dashboard.h"

namespace
{
RayDroneLink link_;
uint32_t next_ui_update_ms_ = 0;
}

void setup()
{
    Serial.begin(115200);
#if P4_ENABLE_DEBUG_LOG
    delay(300);
    Serial.println("\n=== P4SlaveDaisy / RayDrone Visual Link ===");
    Serial.println("Guition JC1060P470C / USB host CDC 0483:5740");
#endif

    // The Arduino core is prebuilt, so sdkconfig.defaults cannot change this
    // watchdog. Keep Core 0 monitored but allow display/USB enumeration bursts
    // without rebooting an instrument in the middle of a session.
    esp_task_wdt_config_t watchdog = {};
    watchdog.timeout_ms = 30000;
    watchdog.idle_core_mask = 1u << 0;
    watchdog.trigger_panic = false;
    esp_task_wdt_reconfigure(&watchdog);

    P4_LOG_PRINTLN("[INIT] MIPI-DSI display");
    if(!display_init())
    {
        P4_LOG_PRINTLN("[FATAL] display init failed");
        while(true)
            delay(1000);
    }

    P4_LOG_PRINTLN("[INIT] LVGL + GT911");
    if(!lvgl_port_init())
    {
        P4_LOG_PRINTLN("[FATAL] LVGL port init failed");
        while(true)
            delay(1000);
    }

    dashboard_create();
    RayDroneModel initial = {};
    initial.link_state = RayDroneLinkState::Searching;
    dashboard_update(initial);
    lvgl_port_start();

    P4_LOG_PRINTLN("[INIT] USB host + RayDrone parser");
    link_.Init();
    P4_LOG_PRINTLN("[READY] Connect Daisy Seed USB-C to the P4 host/OTG port");
}

void loop()
{
    link_.Process();

    const uint32_t now = millis();
    if(static_cast<int32_t>(now - next_ui_update_ms_) >= 0)
    {
        next_ui_update_ms_ = now + 50u;
        if(lvgl_port_lock(8))
        {
            dashboard_update(link_.Model());
            lvgl_port_unlock();
        }
    }

    delay(1);
}
