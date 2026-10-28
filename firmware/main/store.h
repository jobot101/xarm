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
    int16_t  lift[NSERVO];    // added to a grasp pose to get the hover/approach pose
    uint16_t grip_open, grip_close;
    uint16_t move_ms, grip_ms;
    char     slot_name[NSLOT][16];   // spoken names, e.g. "red", "blue"
    uint8_t  n_calib;
    calib_pt_t calib[MAX_CALIB];     // camera pixel -> taught grasp pose
} cfg_t;

extern cfg_t g_cfg;

void store_load(void);   // loads or fills defaults
void store_save(void);
void store_lock(void);
void store_unlock(void);
