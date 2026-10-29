// voice.h — offline speech commands: ES7210 mic -> ESP-SR MultiNet (English) -> router.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "driver/i2c_master.h"

void voice_start(i2c_master_bus_handle_t bus);
void voice_reload(void);                  // call after slot names change
bool voice_ok(void);
void voice_last(char *out, size_t n);     // last heard phrase
void voice_diag(char *status, size_t ns, int *level, float *prob, int *gain, int *thresh);
void voice_tune(int gain_db, int thresh_pct);   // live + saved; -1 leaves a value alone

typedef struct { char phrase[40]; char why[36]; int prob; int age_s; } voice_note_t;
int voice_notes(voice_note_t *out, int max);   // recent matches, newest first, with what happened ("ok", why ignored)
