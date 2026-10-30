// sd.h — the board's TF card, for Muffin's full-quality voice (sounds_hq.bin in /sdcard/muffin/).
// Mounted at boot, never formatted. Muffin writes to the card only after "Store voice on SD card" is
// switched on in Workshop -> Voice, and then only inside /sdcard/muffin/. Nothing else on the card is touched.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"

#define SD_ROOT     "/sdcard"
#define SD_DIR      SD_ROOT "/muffin"
#define SD_BANK     SD_DIR "/sounds.bin"     // 8.3 names: the FAT driver here has no long file names
#define SD_BANK_TMP SD_DIR "/sounds.tmp"

bool sd_init(i2c_master_bus_handle_t bus);  // mount if a readable card is in; false = no card / can't read it
bool sd_ok(void);
void sd_status(char *out, size_t n);         // "16 GB card, 14.2 GB free" / "no card" / why it can't be used
uint64_t sd_free_bytes(void);
bool sd_voice_wanted(void);                  // the Workshop switch (NVS), default off
void sd_voice_set(bool on);
