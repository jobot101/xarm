// fx.h — personality: LED ring (GPIO38) animations + synthesized robot sounds on the speaker.
// Driven by the same states the web app shows (pick.c set()), plus a few events.
#pragma once

typedef enum { FX_BOOT, FX_HEARD, FX_ERROR } fx_event_t;

void fx_start(void);                   // after audio_init()
void fx_state(const char *state);      // "idle" "searching" "moving" "grasping" "holding" "dropping"
                                       // "not_found" "need_calib" "stopped" "updating"
void fx_event(fx_event_t e);
