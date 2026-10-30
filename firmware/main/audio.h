// audio.h — the board's audio path, shared by voice (mic in) and fx (speaker out).
// One I2S port, duplex: TX std -> ES8311 -> PA -> speaker, RX TDM <- ES7210 <- mics. 16 kHz.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"

#define AUDIO_RATE 16000

bool audio_init(i2c_master_bus_handle_t bus);   // codecs + speaker amp
esp_codec_dev_handle_t audio_in(void);           // mic device (NULL if init failed)
// Mic frames come out interleaved in this AFE layout: "MR" (mic1 + speaker reference) or,
// with both mics on, "MRM" (mic1, reference, mic2). TDM slot order is MIC1, MIC3(ref), MIC2, MIC4.
const char *audio_in_format(void);
int audio_in_channels(void);
int audio_raw_channels(void);       // channels in each frame esp_codec_dev_read() returns (>= audio_in_channels())
bool audio_mic_dual(void);                        // second mic in use (decided at boot)
void audio_mic_dual_save(bool on);                // takes effect on the next boot
void audio_mic_gain(float db);                    // analog gain on the mic(s) only, never the reference
bool audio_play(const int16_t *pcm, size_t samples);   // mono 16 kHz, blocking
void audio_volume(int pct);
int64_t audio_play_end_us(void);                  // esp_timer time the speaker goes quiet (voice ignores itself until then)
