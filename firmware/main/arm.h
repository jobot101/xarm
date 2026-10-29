// arm.h — xArm 1S controller over UART. Thread-safe.
#pragma once
#include "core.h"

void arm_init(void);
// Moves are streamed at 25 Hz along smooth curves (see arm.c); these just set the plan and return.
void arm_move(const uint16_t pos[NSERVO], uint16_t ms);     // all six, min-jerk ease in/out over ms
void arm_move_lin(const uint16_t pos[NSERVO], uint16_t ms); // all six, constant speed (chained path segments)
void arm_move_one(int id, uint16_t pos, uint16_t ms);       // id 1..6, eased
void arm_follow_one(int id, uint16_t pos);                  // chase a target (sliders): speed/accel limited
void arm_drive(const float vel[NSERVO], int keep_ms);       // velocity, units/s; all-zero = ramp down.
                                                            // goes to zero by itself if not renewed in keep_ms
void arm_now(uint16_t out[NSERVO]);                         // setpoint being streamed right now
bool arm_read(uint16_t out[NSERVO]);                      // real positions from servos (blocks <=400 ms)
void arm_torque(bool on);       // off = limp for hand-teaching, on = hold where it is
void arm_hold(void);            // brake to rest where it is (used by stop); from limp: power up in place
bool arm_torque_on(void);
void arm_last(uint16_t out[NSERVO]);   // where each joint is headed (its target), or where it is if idle
int  arm_battery_mv(void);
bool arm_link_ok(void);
void arm_limits(uint16_t out[NSERVO][2]);          // [i][0]=min, [i][1]=max, servo index 0..5
void arm_limits_set(int i, int lo, int hi);        // saved to NVS; every move is clamped to these         // got any reply in the last 5 s
