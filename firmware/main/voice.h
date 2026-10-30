// voice.h — offline speech commands: ES7210 mic -> ESP-SR AFE (AEC, AGC, VADNet) -> MultiNet7 (English) -> router.
// "stop" always works; everything else after "Hey Muffin" (switchable in Workshop -> Voice).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "driver/i2c_master.h"

void voice_start(i2c_master_bus_handle_t bus);
void voice_reload(void);                  // call after slot names change
bool voice_ok(void);
void voice_last(char *out, size_t n);     // last heard phrase
void voice_diag(char *status, size_t ns, int *level, float *prob, int *gain, int *thresh);
void voice_tune(int gain_db, int thresh_pct, int wake);   // live + saved; -1 leaves a value alone
void voice_diag2(bool *wake, bool *open, const char **vad);  // "Hey Muffin" gate on, window open now, "VADNet"/"WebRTC"
void voice_engage(void);                  // someone's talking to Muffin (app tap, Muffin asked a question): open the window
int  voice_face(void);                    // turn toward voices: 1 on, -1 on (mics mounted the other way), 0 off
void voice_face_set(int v);               // saved; needs "Use both mics"
// music in the room? its beat period and when the next beat lands (esp_timer us); false = no clear beat
bool voice_beat(int *period_ms, int64_t *next_us);
void voice_hold_open(bool on);            // School's in class: ears stay open, no "Hey Muffin" needed

typedef struct { char phrase[64]; char why[48]; int prob; int age_s; } voice_note_t;
int voice_notes(voice_note_t *out, int max);   // recent matches, newest first, with what happened ("ok", why ignored)
