// school.h — Muffin's Robot School: a self-running daily class, voice only.
//
// Once a day each kid gets a class of N minutes (Workshop -> School, default 20). The first thing anyone says
// starts it: Muffin greets them, asks who's there if it knows several kids, runs a warm-up quiz on past
// lessons (missed questions first), teaches the next lesson (talking while the arm demonstrates, with
// "your turn" moments where the kid gives a voice command), checks it with two true/false questions, offers
// another lesson while there's time, then free play. When the time is up it finishes the sentence, says
// what's next tomorrow, bows, and sleeps until the next day: it only answers "stop" and the grown-up
// passphrase (ten more minutes). The web app is the grown-up's controller and is never limited.
//
// Lessons and School's lines live on the SD card bank (tools/school_lessons.py); with no card, Muffin says
// so and the class is just timed free play. Day boundaries come from the clock: internet time when there's
// WiFi, else the board's RTC (PCF85063) if it kept time; with neither, each boot counts as a new day.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "cJSON.h"

void school_init(i2c_master_bus_handle_t bus);   // after kids_init(), fx_start(), pick_init()
void school_clock_start(void);                   // after net_start(): internet time keeps the RTC right

// router.c (voice only; app commands skip School):
bool school_filter(const char *t, char *msg, size_t n);       // before everything else but "stop": true = handled
bool school_allow(int kind, char *msg, size_t n);              // a command of cmd_kind_t `kind` may run now?
void school_note_stop(void);                                   // someone said stop
const char *school_pass(void);                                 // grown-up passphrase ("" = none) for MultiNet

bool school_explain(int lesson, char *msg, size_t n);    // a robots/math/physics question: tell that lesson (not mid-class)
bool school_asleep(void);         // pick.c idle life: head down
bool school_wrap_due(void);       // pick.c: time's up, finish the sentence and stop
bool school_chatter_ok(void);
bool school_in_lesson(void);      // router.c: in class and not free play (chat's own tricks and lessons wait)     // fx.c: idle chatter only outside class and sleep
void school_kid_reset(int kid);   // kids.c: a kid slot was freed or reused

// web.c: GET /api/school, POST /api/school
void school_json(cJSON *o);
bool school_set(const cJSON *b, char *msg, size_t n);
