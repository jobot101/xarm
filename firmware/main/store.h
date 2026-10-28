// store.h — persistent config (NVS blob). Everything the web app can teach/edit lives here.
#pragma once
#include "core.h"

#define MAX_CALIB 12
#define NSLOT 2               // vision module color mode gives two color slots

typedef struct {
    uint32_t magic;
    uint16_t home[NSERVO];    // safe carry pose
    uint16_t drop[NSERVO];    // where "drop it" puts things
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
