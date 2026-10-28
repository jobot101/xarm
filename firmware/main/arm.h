// arm.h — xArm 1S controller over UART. Thread-safe.
#pragma once
#include "core.h"

void arm_init(void);
void arm_move(const uint16_t pos[NSERVO], uint16_t ms);   // all six
void arm_move_one(int id, uint16_t pos, uint16_t ms);     // id 1..6
bool arm_read(uint16_t out[NSERVO]);                      // real positions from servos (blocks <=400 ms)
void arm_torque(bool on);       // off = limp for hand-teaching, on = hold where it is
void arm_hold(void);            // freeze in place (used by stop)
bool arm_torque_on(void);
void arm_last(uint16_t out[NSERVO]);   // last commanded / last read
int  arm_battery_mv(void);
bool arm_link_ok(void);
void arm_limits(uint16_t out[NSERVO][2]);          // [i][0]=min, [i][1]=max, servo index 0..5
void arm_limits_set(int i, int lo, int hi);        // saved to NVS; every move is clamped to these         // got any reply in the last 5 s
