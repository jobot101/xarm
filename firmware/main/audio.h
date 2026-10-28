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
bool audio_play(const int16_t *pcm, size_t samples);   // mono 16 kHz, blocking
void audio_volume(int pct);
