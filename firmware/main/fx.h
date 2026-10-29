// fx.h — Ragamuffin Hall personality: the LED ring (GPIO38) is a little rose window in
// stained-glass jewels, and the speaker plays church bells, organ and a voice — all pre-rendered by
// tools/make_sounds.py into sounds.bin (groups are the SND_* names in sounds_gen.h).
// Driven by the same states the web app shows (pick.c set()), plus a few events.
#pragma once
#include "sounds_gen.h"

typedef enum { FX_BOOT, FX_HEARD, FX_ERROR, FX_BELL, FX_CHEER,
               FX_UPDATED } fx_event_t;   // FX_UPDATED: first boot of a new firmware -> fanfare + "Ready to rumble!"

void fx_start(void);                   // after audio_init()
void fx_state(const char *state);      // idle searching moving grasping holding dropping giving sorting
                                       // waving bowing dancing ringing playing not_found need_calib stopped updating
void fx_event(fx_event_t e);
void fx_show(int jewel);               // game cue: 0 ruby, 1 sapphire, 2 emerald, 3 gold — whole ring + its note
void fx_greet(int jewel, unsigned seed);   // a kid arrives: ring in their jewel + their own 3-note jingle

// talking: one random line from a group (SND_*), never the same twice running. Lines that can't start
// within ~2.5 s are dropped rather than narrating something that's already over.
void fx_say(int group);
void fx_maybe(int group, int pct);                              // say it pct% of the time
void fx_say_color(int color_first, int plain, const char *name);   // SND_X_RED.. if name is a camera color, else plain
void fx_say_n(int first, int n);                                // SND_X_1 + n-1
void fx_hush(void);                                             // cut the current clip, drop the queue
