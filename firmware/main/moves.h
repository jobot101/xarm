// moves.h — record a move by hand (arm limp, sampled at 5 Hz) and keep it as a named, replayable trick.
// Stored in the "store" NVS partition (256 KB), separate from config.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "core.h"

#define MOVE_MAX     8      // saved moves
#define MOVE_FRAMES  150    // 30 s at 5 Hz
#define MOVE_HZ      5

void moves_init(void);
bool moves_record_start(void);              // goes limp and starts sampling
bool moves_record_save(const char *name);   // stops, holds, stores. false if nothing recorded / full
void moves_record_cancel(void);
bool moves_recording(int *frames);
int  moves_count(void);
const char *moves_name(int i);
int  moves_find(const char *text);          // index of a saved move whose name appears in text, or -1
int  moves_load(int i, uint16_t (*frames)[NSERVO], int max);   // returns frame count
bool moves_delete(int i);
