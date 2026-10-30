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

// ---- smooth motion (arm.c streams setpoints at a fixed rate; these compute one tick) ----
// minimum-jerk ease: s in 0..1 -> 0..1, zero velocity and acceleration at both ends
float ease_minjerk(float s);
// chase `target` with velocity <= vmax and acceleration <= amax (units/s, units/s^2), braking so it
// lands without overshoot. Updates *pos/*vel by dt seconds. Returns true once settled on target.
bool follow_step(float *pos, float *vel, float target, float vmax, float amax, float dt);
// ramp *vel toward vdes at <= amax, then integrate *pos
void vel_step(float *pos, float *vel, float vdes, float amax, float dt);
// Catmull-Rom point between p1 and p2 (t 0..1): smooth path through recorded frames
float catmull(float p0, float p1, float p2, float p3, float t);

// ---- lifelike travel (pick.c travel()): one pose of a move that arcs and lets the big joints lead.
// Each joint eases (min-jerk) from `from` to `to` over dur_ms, starting lag_ms[i] late (overlapping action:
// base first, gripper last), and bulges by `bulge[i]` at mid-flight (an arc: the hand rises through the
// middle instead of sliding straight). t_ms runs 0 .. dur_ms + max lag. Ends exactly on `to`.
void arc_pose(const uint16_t from[NSERVO], const uint16_t to[NSERVO], const int16_t bulge[NSERVO],
              const uint16_t lag_ms[NSERVO], float dur_ms, float t_ms, uint16_t out[NSERVO]);

// pink (1/f) noise, about -1..1: the irregular drift living things have (Voss-McCartney, 8 rows). Idle life
// wobbles with it instead of a clean sine.
typedef struct { float row[8]; float sum; uint32_t n; } pink_t;
float pink_next(pink_t *p, uint32_t rnd);

// tempo from an onset envelope (env[i] = how much louder frame i got, frames hop_ms apart, oldest first):
// the beat period in frames between lo and hi with the strongest autocorrelation, and how sure (0..1).
// 0 if there's no beat. *phase = frames from the newest frame back to the last beat.
int beat_find(const float *env, int n, int lo, int hi, float *conf, int *phase);

// ---- smarter picking ----
// wrist auto-align: jaws_axis = how the jaws close in the calibrated grasp, as seen by the camera
// (0 off, 1 across image X, 2 across image Y). Returns the wrist-rotate delta: `turn` when the object is
// long (>= 1.4x) along the axis the jaws would squeeze, else 0.
int  align_twist(int w, int h, int jaws_axis, int turn);
// grab check: true if the gripper closed to within `margin` of fully closed — nothing in the hand
bool grip_missed(int actual, int grip_close, int grip_open, int margin);
// size sort: 0 = small, 1 = big (blob box area vs split, px^2)
int  size_bin(int w, int h, int split);

// ---- cloud speech (Whisper transcript -> router text) ----
// "Hey Muffin, can you pick up the RED one, please?" -> "pick up the red one". Lower-cases, turns
// punctuation into spaces, strips a leading wake word and politeness ("can you", "please", "um"...), and
// folds one-word answers onto what chat.c expects ("yeah sure!" -> "yes", "nah" -> "no").
// Returns TC_WAKE if "muffin" was in it; out may be "" (they only said its name).
#define TC_WAKE 1
int  text_clean(const char *in, char *out, size_t n);
// Whisper's stock hallucinations on noise ("thanks for watching", a lone "you"): true = throw away
bool text_junk(const char *cleaned);

// ---- Robot School (school.c) ----
// What a command does, as the router classifies it. School "your turn" steps name one of these, so
// tools/make_sounds.py reads this enum to check the lesson scripts: keep names and order in step.
// K_NUDGE in a script accepts any nudge (up/down/left/right/turn/tilt).
// K_FETCH / K_GIVEBACK are School-only turns (never a command): "hand me something" is answered by a clamp
// (K_CLOSE) and "take it back" by a let go (K_OPEN). make_sounds.py parses this enum: no comments inside it.
typedef enum { K_NONE, K_NUDGE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_TURN, K_TILT, K_OPEN, K_CLOSE, K_BELL,
               K_DANCE, K_WAVE, K_BOW, K_PICK, K_HOLD, K_STOP, K_HOME, K_DROP, K_GIVE, K_SORT, K_STACK,
               K_PLAY, K_FETCH, K_GIVEBACK, K_KINDS } cmd_kind_t;
bool kind_fits(int want, int got);              // does a command of kind `got` answer a turn that wants `want`?
// days since 1970-01-01 for a calendar date (proleptic Gregorian); yesterday = today - 1, across years too
int32_t civil_days(int y, int m, int d);
// streak after a visit on `today`, given the day of the previous visit
uint16_t streak_next(int32_t last_day, int32_t today, uint16_t streak);
// grown-up passphrase: true if `heard` contains `pass` as whole words (case, punctuation and extra
// spaces ignored). A pass shorter than 6 letters never matches, so a blank or toy one can't unlock.
bool pass_heard(const char *heard, const char *pass);
// pick a review question among the first `n` learned ones: a missed one (bit set) if any, else any;
// never `avoid` unless it's the only one. -1 if n == 0.
int  recap_pick(const uint8_t *missed, int n, uint32_t rnd, int avoid);
#define SCHOOL_RANK_EVERY 5                     // a new rank every 5 lessons finished
int  rank_of(int lessons_done);                 // 0 = Apprentice, +1 every SCHOOL_RANK_EVERY lessons
