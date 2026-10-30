// fx.h — Ragamuffin Hall personality: the LED ring (GPIO38) is a little rose window in
// stained-glass jewels, and the speaker plays church bells, organ and Muffin's voice — all pre-rendered by
// tools/make_sounds.py into sounds.bin (groups are the SND_* names in sounds_gen.h), which lives in the
// "model" partition at FX_BANK_OFF and is kept current over WiFi by update.c.
// Driven by the same states the web app shows (pick.c set()), plus a few events.
#pragma once
#include <stdbool.h>
#include "sounds_gen.h"

#include <stdint.h>

// The bank sits inside "model", after the speech models. Two layouts exist in the field:
//   7 MB "model" (Oct 2026 on): srmodels.bin (MultiNet7 + VADNet, ~3.05 MB) gets 3.5 MB, bank at 0x380000
//   6 MB "model" (before):      srmodels.bin (MultiNet7 only, ~2.76 MB), bank at 0x2C0000
// The app reads the partition size and picks, so an OTA'd app on an old board keeps talking until the
// next USB install moves everything. CI checks srmodels.bin fits under FX_BANK_OFF.
#define FX_BANK_OFF     0x380000
#define FX_BANK_OFF_OLD 0x2C0000
#define FX_BANK_MAX     0x340000  // = BUDGET in tools/make_sounds.py; fits both layouts
static inline uint32_t fx_bank_off(uint32_t model_size)
{
    return model_size >= FX_BANK_OFF + FX_BANK_MAX ? FX_BANK_OFF : FX_BANK_OFF_OLD;
}

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
void fx_play(int group);                                        // a sound effect group (SND_S_*)
bool fx_bank_ok(void);                                          // a matching sound bank is loaded
const char *fx_bank_src(void);                                  // "sd" (full quality) / "flash" / "none"
void fx_bank_release(void);                                     // before rewriting the bank in flash
void fx_bank_reload(void);                                      // after
void fx_quiet(int minutes);                                     // "be quiet": no talking for a while (0 = talk again)
bool fx_is_quiet(void);
bool fx_speaking(void);
void fx_listening(bool on);                                     // "Hey Muffin" window open: ring glows sapphire when idle                                         // something is playing or queued
