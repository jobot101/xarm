// radar.h — a mmWave presence radar on one GPIO (Johnny: IO7). CONFIG_XARM_RADAR_GPIO = -1 means none.
//
// Hi-Link modules either drive a plain presence output (high = someone there) or send UART frames on that pin.
// radar.c doesn't assume: it counts edges per second. A few edges and a steady level = presence output;
// hundreds or more = serial data (then it reports "serial" and the level means nothing).
#pragma once
#include <stdbool.h>
#include "cJSON.h"

void radar_init(void);
bool radar_present(void);          // presence output high (false if none, or if the pin is carrying serial data)
void radar_json(cJSON *o);         // /api/state "radar": {gpio, level, edges_s, kind, present, since_s}
