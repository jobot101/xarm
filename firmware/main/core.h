// core.h — pure logic, no ESP-IDF deps. Unit-tested on host (firmware/test).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define NSERVO 6            // xArm 1S: id1 gripper .. id6 base. index = id-1
#define POS_MIN 0
#define POS_MAX 1000

// ---- xArm 1S bus-servo controller protocol (UART 9600 8N1) ----
// frame: 0x55 0x55 LEN CMD PARAMS...   LEN = 2 + nparams (counts LEN+CMD)
enum {
    XCMD_SERVO_MOVE     = 3,
    XCMD_ACTION_RUN     = 6,
    XCMD_ACTION_STOP    = 7,
    XCMD_ACTION_DONE    = 8,
    XCMD_BATTERY        = 15,
    XCMD_UNLOAD         = 20,
    XCMD_POS_READ       = 21,
};

size_t xf_move(uint8_t *out, const uint8_t *ids, const uint16_t *pos, int n, uint16_t ms);
size_t xf_ids(uint8_t *out, uint8_t cmd, const uint8_t *ids, int n);   // UNLOAD / POS_READ
size_t xf_simple(uint8_t *out, uint8_t cmd);                           // BATTERY / ACTION_STOP
size_t xf_action(uint8_t *out, uint8_t group, uint16_t times);

typedef struct {
    int st, need, got;
    uint8_t cmd;
    uint8_t p[64];          // params
    int np;
} xparser_t;
// feed one byte; returns true when a full frame is in p->cmd / p->p / p->np
bool xp_feed(xparser_t *p, uint8_t b);
// decode POS_READ reply params into pos[id-1]; returns count decoded
int xp_positions(const xparser_t *p, uint16_t pos[NSERVO]);

// ---- Hiwonder ESP32-S3 vision module, color mode ----
// I2C 0x52, reg 0x00 = color slot 0, reg 0x01 = slot 1. 4 bytes: uint8 x,y,w,h (center + size, px,
// 240x240 frame) — per Hiwonder's sketch source (cam/hiwonder), which settles the docs' 4-vs-8-byte conflict
typedef struct { int16_t x, y, w, h; } box_t;
#define CAM_BYTES 4
#define CAM_FRAME 240
bool cam_decode(const uint8_t raw[CAM_BYTES], box_t *b);   // true if a blob is present

// ---- pixel -> joint pose interpolation ----
typedef struct { int16_t px, py; uint16_t pos[NSERVO]; } calib_pt_t;
// inverse-distance-weighted blend of taught grasp poses. needs n >= 1.
void interp_pose(const calib_pt_t *pts, int n, float px, float py, uint16_t out[NSERVO]);

uint16_t clamp_pos(int v);

// ---- smarter picking ----
// wrist auto-align: jaws_axis = how the jaws close in the calibrated grasp, as seen by the camera
// (0 off, 1 across image X, 2 across image Y). Returns the wrist-rotate delta: `turn` when the object is
// long (>= 1.4x) along the axis the jaws would squeeze, else 0.
int  align_twist(int w, int h, int jaws_axis, int turn);
// grab check: true if the gripper closed to within `margin` of fully closed — nothing in the hand
bool grip_missed(int actual, int grip_close, int grip_open, int margin);
// size sort: 0 = small, 1 = big (blob box area vs split, px^2)
int  size_bin(int w, int h, int split);
