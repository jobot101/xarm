// update.h — auto-update: the board watches the build CI publishes to GitHub Pages
// (manifest.json + xarm-voice.bin) and installs any new version over HTTPS.
// Loop: change code -> push -> CI builds (~3 min) -> board picks it up on its next check.
#pragma once
#include <stdbool.h>
#include <stddef.h>

bool update_new_version(void);          // true once on the first boot of a new build (USB or OTA)
void update_start(void);                 // background task; needs net_online()
void update_check_now(void);             // wake the task for an immediate check
void update_set_auto(bool on);           // persisted
bool update_auto(void);
void update_status(char *latest, size_t nl, char *status, size_t ns);
void update_sound_status(char *out, size_t n);   // Muffin's voice bank: ready / downloading / failed
