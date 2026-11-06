// radar.h — a mmWave presence radar on one GPIO (MotherShip: IO7). CONFIG_XARM_RADAR_GPIO = -1 means none.
//
// Hi-Link modules either drive a plain presence output (high = someone there) or send UART frames on that pin.
// radar.c doesn't assume: it counts edges per second. A few edges and a steady level = presence output;
// hundreds or more = serial data (then it reports "serial" and the level means nothing).
#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include <stdint.h>

void radar_init(void);
bool radar_known(void);            // a reading we understand (presence pin, or a recognized serial format)
int  radar_away_s(void);           // seconds since someone was last seen (0 while someone's here, -1 if unknown)
int  radar_distance_cm(void);      // -1 if nobody or not reported
bool radar_angle(int *deg);        // LD2450 only: nearest person's angle (0 ahead, + right); false if no direction
bool radar_present(void);          // presence output high (false if none, or if the pin is carrying serial data)
void radar_json(cJSON *o);
// ignore zones: rectangles {x0, x1, y0, y1} in cm (x + = to the right, y = straight out) where targets are clutter,
// e.g. the arm's own idle motion. Saved in NVS; up to 4. Returns how many were kept.
int  radar_zones_set(const int16_t z[][4], int n);         // /api/state "radar": {gpio, level, edges_s, kind, present, since_s}
