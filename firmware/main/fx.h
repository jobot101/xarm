// fx.h — Ragamuffin Hall personality: the LED ring (GPIO38) is a little rose window in
// stained-glass jewels, and the speaker plays church bells + organ phrases (synthesized on the fly).
// Driven by the same states the web app shows (pick.c set()), plus a few events.
#pragma once

typedef enum { FX_BOOT, FX_HEARD, FX_ERROR, FX_BELL, FX_CHEER } fx_event_t;

void fx_start(void);                   // after audio_init()
void fx_state(const char *state);      // idle searching moving grasping holding dropping giving sorting
                                       // waving bowing dancing ringing playing not_found need_calib stopped updating
void fx_event(fx_event_t e);
void fx_show(int jewel);               // game cue: 0 ruby, 1 sapphire, 2 emerald, 3 gold — whole ring + its note
