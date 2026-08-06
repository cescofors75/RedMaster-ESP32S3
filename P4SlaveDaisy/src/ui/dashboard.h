#pragma once

#include "raydrone_link.h"

void dashboard_create();
void dashboard_update(const RayDroneModel& model);
bool dashboard_take_focus_command(uint16_t* focus_milli);
bool dashboard_take_command(raydrone_usb::CommandId* id, uint16_t* value);
