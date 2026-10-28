// store.h — persistent config (NVS blob). Everything the web app can teach/edit lives here.
#pragma once
#include "core.h"

#define MAX_CALIB 12
#define NSLOT 2               // vision module color mode gives two color slots

typedef struct {
    uint32_t magic;
    uint16_t home[NSERVO];    // safe carry pose
    uint16_t drop[NSERVO];    // where "drop it" puts things
    uint16_t give[NSERVO];    // hand-off pose: "give it to me"
    uint16_t bell[NSERVO];    // striking pose for "ring the bell"
    uint16_t bin[2][NSERVO];  // per-color drop poses for "sort"
    uint16_t look[NSERVO];    // head cam: pose the arm takes to look down at the mat (calibration is taught from here)
    int16_t  scan[3];         // head cam: base offsets to look around (center, left, right)
    uint8_t  cam_on_head;     // 1 = camera rides on the wrist, 0 = fixed overhead
    uint16_t helper[NSERVO];  // third hand: where "hold this" presents the open gripper
    uint16_t tower[NSERVO];   // stacking: pose that sets the FIRST block down on the tower spot
    int16_t  stack_pct;       // stacking: each level adds this % of the lift offset (block height)
    int16_t  lift[NSERVO];    // added to a grasp pose to get the hover/approach pose
    uint16_t grip_open, grip_close;
    uint16_t move_ms, grip_ms;
    char     slot_name[NSLOT][16];   // spoken names, e.g. "red", "blue"
    uint8_t  n_calib;
    calib_pt_t calib[MAX_CALIB];     // camera pixel -> taught grasp pose
} cfg_t;

extern cfg_t g_cfg;
// drive "reach" direction (forward/back) — kept outside cfg_t so adding it didn't reset anyone's setup
extern int16_t g_reach[NSERVO];
void store_reach_save(void);

// smarter picking settings — also outside cfg_t so adding them didn't reset anyone's setup
typedef struct {
    uint8_t  align_axis;     // wrist auto-align: 0 off, 1 jaws close across image X, 2 across image Y
    int16_t  align_turn;     // wrist-rotate counts for a 90° turn (LX servos: ~375)
    uint8_t  grab_check;     // 1 = verify the grip, retry on a miss
    int16_t  grab_margin;    // counts short of fully closed that still means "holding something"
    uint8_t  grab_tries;
    int16_t  size_split;     // sort by size: blob area px^2 at/above which it's "big"
} pick2_t;
extern pick2_t g_pick2;
void store_pick2_save(void);

void store_load(void);   // loads or fills defaults
void store_save(void);
void store_lock(void);
void store_unlock(void);
