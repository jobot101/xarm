// cloud.h — "cloud ears": after "Hey Muffin", the utterance goes to a Whisper speech-to-text API (Groq's
// free tier by default, any OpenAI-compatible /audio/transcriptions endpoint via Kconfig) and the text
// goes through router_exec like anything else. "Stop" and the wake word stay on the board (MultiNet),
// and with no key / no internet / API trouble, MultiNet does everything as before.
//
// The key lives in NVS only (the repo is public): Workshop -> Voice -> Groq key.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLOUD_RATE      16000
#define CLOUD_MAX_MS    8000          // longest utterance sent

typedef struct {
    int16_t *pcm;                     // mono 16 kHz, AFE output (echo-cancelled)
    int n;                            // samples
    bool wake;                        // MultiNet heard "muffin" in it, or the window was already open
    bool stopped;                     // MultiNet already ran "stop" for it — don't run it twice
    char fallback[48];                // MultiNet's guess for it, used if the cloud call fails
    float fallback_prob;
} cloud_clip_t;

void cloud_init(void);
bool cloud_ready(void);               // key set, switched on, online, not backing off, under the daily cap
bool cloud_submit(const cloud_clip_t *c);   // copies the clip; false if a request is still in flight
bool cloud_busy(void);

void cloud_set_key(const char *key);  // "" clears it
void cloud_enable(bool on);
// for /api/state: on (switch), key ("gsk_…wxyz" or ""), status line, last transcript, its round trip, requests today
void cloud_diag(bool *on, char *key_hint, size_t nk, char *status, size_t ns, char *last, size_t nl, int *ms, int *today);
