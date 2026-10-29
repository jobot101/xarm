// Mic path ported from xiaozhi-esp32 boards/waveshare/esp32-s3-audio-board + codecs/box_audio_codec.cc,
// AFE setup follows xiaozhi audio/engines/afe_audio_engine.cc (same esp-sr 2.4.7 pin).
//
//   ES7210 (mic1 + speaker reference [+ mic2]) -> AFE (AEC, BSS, AGC, VAD) -> MultiNet7 -> router
//
// AEC subtracts the board's own bells/organ/voice, so "stop" works while a trick is playing.
// MultiNet runs continuously, so "stop" is always instant. Everything else waits for "Hey Muffin": that
// opens an ~8 s listening window (ring glows sapphire), each command heard keeps it open, and it stays
// open while Muffin is talking plus a few seconds after, so a conversation flows without repeating the
// name. A command only counts if the VAD (VADNet, a trained model; WebRTC if the model isn't in flash)
// heard real speech in the last XARM_VOICE_VAD_HOLD_MS, which kills most servo-noise triggers.
#include "voice.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "router.h"
#include "cloud.h"
#include "core.h"
#include "audio.h"
#include "fx.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_codec_dev.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_vadn_models.h"
#include "model_path.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define MAX_PHRASES 200          // MultiNet7 takes up to 300

static const char *TAG = "voice";
static esp_codec_dev_handle_t s_in;
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afed;
static const esp_mn_iface_t *s_mn;
static model_iface_data_t *s_mnd;
static char s_phr[MAX_PHRASES][48];
static volatile bool s_ok, s_reload;
static char s_last[64] = "", s_status[64] = "starting";
static volatile int s_level;          // raw mic level 0..100 (dBFS mapped, peak-hold)
static float s_peak;
static volatile float s_prob;         // confidence of the last recognized phrase
static volatile int64_t s_speech_us;  // last time the VAD said "speech"
static int s_gain = CONFIG_XARM_MIC_GAIN_DB, s_thresh = CONFIG_XARM_VOICE_THRESHOLD;
static bool s_wake = true;            // "Hey Muffin" first (stop words always pass)
static volatile int64_t s_open_until; // listening window end (esp_timer us)
static volatile int64_t s_engaged;    // last time anyone talked to Muffin (voice or app)
static char s_vad[12] = "—";          // which VAD the AFE ended up with

#define WAKE_WINDOW_US  (8 * 1000000LL)    // after "Hey Muffin" or any command heard
#define FOLLOW_US       (6 * 1000000LL)    // after Muffin stops talking, in an active conversation
#define SESSION_US      (60 * 1000000LL)   // "active conversation" = someone engaged within this


// runtime tuning, saved in NVS so the app can adjust without a rebuild
static void tune_load(void)
{
    nvs_handle_t h;
    if (nvs_open("xarm", NVS_READONLY, &h) != ESP_OK) return;
    int32_t v;
    if (nvs_get_i32(h, "mic_gain", &v) == ESP_OK) s_gain = v;
    if (nvs_get_i32(h, "mn_thresh", &v) == ESP_OK) s_thresh = v;
    uint8_t w;
    if (nvs_get_u8(h, "wake", &w) == ESP_OK) s_wake = w;
    nvs_close(h);
}

static void tune_apply(void)
{
    audio_mic_gain((float)s_gain);
    if (s_mn && s_mnd) s_mn->set_det_threshold(s_mnd, s_thresh / 100.0f);
}

void voice_tune(int gain_db, int thresh_pct, int wake)
{
    if (gain_db >= 0 && gain_db <= 42) s_gain = gain_db;
    if (thresh_pct >= 5 && thresh_pct <= 90) s_thresh = thresh_pct;
    if (wake == 0 || wake == 1) s_wake = wake;
    tune_apply();
    nvs_handle_t h;
    if (nvs_open("xarm", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "mic_gain", s_gain);
        nvs_set_i32(h, "mn_thresh", s_thresh);
        nvs_set_u8(h, "wake", s_wake);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void load_commands(void)
{
    int n = router_phrases(s_phr, MAX_PHRASES);
    esp_mn_commands_clear();
    int added = 0;
    for (int i = 0; i < n; i++) if (esp_mn_commands_add(i + 1, s_phr[i]) == ESP_OK) added++;
    esp_mn_error_t *err = esp_mn_commands_update();
    int bad = err ? err->num : 0;
    if (bad) ESP_LOGW(TAG, "%d phrases rejected by MultiNet", bad);
    s_mn->print_active_speech_commands(s_mnd);
    snprintf(s_status, sizeof s_status, "listening — %d phrases%s", added - bad, bad ? " (some rejected)" : "");
    ESP_LOGI(TAG, "%s", s_status);
}

// stop words stay live while the speaker plays; everything else waits for quiet
static bool is_stop(const char *p) { return strstr(p, "stop") || strstr(p, "freeze"); }
// "hey muffin", "hi muffin", "hello muffin" — but not "stop muffin"
static bool is_wake(const char *p) { return strstr(p, "muffin") && !is_stop(p); }

// The window: open after a wake phrase or command, and in a live conversation also while Muffin talks
// and a few seconds after (so "yes please" to Muffin's question works without the name).
static bool window_open(int64_t now)
{
    if (now < s_open_until) return true;
    return now - s_engaged < SESSION_US && now < audio_play_end_us() + FOLLOW_US;
}

void voice_engage(void)
{
    int64_t now = esp_timer_get_time();
    s_engaged = now;
    if (s_open_until < now + WAKE_WINDOW_US) s_open_until = now + WAKE_WINDOW_US;
}

// the last few things it matched, kept or not, and why — shown in the app's Voice panel for tuning
#define NOTES 10
static struct { char p[64]; char why[48]; uint8_t prob; uint32_t age_s; int64_t t; } s_notes[NOTES];
static int s_note_n;
static portMUX_TYPE s_note_mux = portMUX_INITIALIZER_UNLOCKED;

static void note(const char *p, float pr, const char *why)
{
    portENTER_CRITICAL(&s_note_mux);
    int i = s_note_n++ % NOTES;
    strlcpy(s_notes[i].p, p, sizeof s_notes[i].p);
    strlcpy(s_notes[i].why, why, sizeof s_notes[i].why);
    s_notes[i].prob = (uint8_t)(pr * 100);
    s_notes[i].t = esp_timer_get_time();
    portEXIT_CRITICAL(&s_note_mux);
}

int voice_notes(voice_note_t *out, int max)
{
    int n = 0;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_note_mux);
    for (int k = 0; k < NOTES && n < max && k < s_note_n; k++) {
        int i = (s_note_n - 1 - k) % NOTES;                    // newest first
        strlcpy(out[n].phrase, s_notes[i].p, sizeof out[n].phrase);
        strlcpy(out[n].why, s_notes[i].why, sizeof out[n].why);
        out[n].prob = s_notes[i].prob;
        out[n].age_s = (int)((now - s_notes[i].t) / 1000000);
        n++;
    }
    portEXIT_CRITICAL(&s_note_mux);
    return n;
}

// ---------------------------------------------------------------- cloud ears: utterance recorder
// While the cloud is usable, every stretch of speech the VAD marks is recorded (with ~320 ms of pre-roll, so
// the first word isn't clipped). When it ends, it's sent to Whisper if Muffin was listening: the window was
// open, or MultiNet caught "Hey Muffin" inside it. MultiNet still runs on everything: it does "stop" on the
// spot, opens the window on the wake word, and its guess is the fallback if the cloud call fails.
#define SEG_MAX  (CLOUD_RATE * CLOUD_MAX_MS / 1000)
#define PRE_N    (CLOUD_RATE * 320 / 1000)
#define SEG_MIN  (CLOUD_RATE * 350 / 1000)
static int16_t *s_seg, *s_pre;       // both in PSRAM
static int s_seg_n, s_pre_i;
static bool s_seg_on, s_seg_wake, s_seg_stopped;
static int64_t s_seg_t0;
static char s_seg_fb[48];
static float s_seg_fbp;
static int64_t s_huh_at, s_sent_at;

static void run_text(const char *t, float pr, const char *tag);

static void seg_feed(const int16_t *d, int n, bool speech, int64_t now)
{
    if (!s_seg_on) {
        for (int i = 0; i < n; i++) { s_pre[s_pre_i] = d[i]; s_pre_i = (s_pre_i + 1) % PRE_N; }
        if (!speech) return;
        s_seg_on = true;
        s_seg_t0 = now;
        s_seg_wake = !s_wake || window_open(now);
        s_seg_stopped = false;
        s_seg_fb[0] = 0; s_seg_fbp = 0;
        // pre-roll, oldest first (it already holds this frame, so it isn't appended again)
        for (int i = 0; i < PRE_N; i++) s_seg[i] = s_pre[(s_pre_i + i) % PRE_N];
        s_seg_n = PRE_N;
        return;
    }
    int take = n < SEG_MAX - s_seg_n ? n : SEG_MAX - s_seg_n;
    if (take > 0) { memcpy(s_seg + s_seg_n, d, take * sizeof(int16_t)); s_seg_n += take; }
    if (speech && s_seg_n < SEG_MAX) return;

    // the end of an utterance (the VAD only says silence after ~0.5 s of it, so the tail is included)
    s_seg_on = false;
    if (window_open(now)) s_seg_wake = true;     // "Hey Muffin" may have opened it on the way
    if (s_seg_n < PRE_N + SEG_MIN || !s_seg_wake) return;
    // started while Muffin was talking and ended right after: that's Muffin's own voice leaking past the AEC
    int64_t play_end = audio_play_end_us();
    if (s_seg_t0 < play_end && now < play_end + 400000 && !s_seg_fb[0]) return;
    cloud_clip_t c = { .pcm = s_seg, .n = s_seg_n, .wake = s_seg_wake, .stopped = s_seg_stopped, .fallback_prob = s_seg_fbp };
    strlcpy(c.fallback, s_seg_fb, sizeof c.fallback);
    s_sent_at = now;
    if (!cloud_submit(&c)) {
        if (s_seg_fb[0]) run_text(s_seg_fb, s_seg_fbp, "");      // still waiting on the last one: use MultiNet's guess
        else note("(too fast)", 0, "still thinking about the last one");
    }
}

// cloud.c calls this from its task with Whisper's text (err = HTTP status or -1 when it failed)
void voice_cloud_heard(const char *text, int err, const cloud_clip_t *c)
{
    if (err) {
        if (c->fallback[0] && c->fallback_prob >= s_thresh / 100.0f) run_text(c->fallback, c->fallback_prob, "on-board: ");
        else note("(cloud didn't answer)", 0, err == 429 ? "Groq's free limit — on-board for 1 min" : "on-board ears for a bit");
        return;
    }
    char t[96];
    int f = text_clean(text, t, sizeof t);
    if (!t[0] && (f & TC_WAKE)) strcpy(t, "hey muffin");          // only said its name: it answers to that
    if (text_junk(t)) { note(text[0] ? text : "(nothing)", 0, "cloud: no words in it"); return; }
    if (c->stopped && (strstr(t, "stop") || strstr(t, "freeze"))) { note(t, 100, "stop (already done)"); return; }
    run_text(t, 1.0f, "☁ ");
}

// one command, from either ear
static void run_text(const char *t, float pr, const char *tag)
{
    char msg[64], why[48];
    voice_engage();
    strlcpy(s_last, t, sizeof s_last);
    s_prob = pr;
    bool ok = router_exec(t, msg, sizeof msg);
    if (!ok && !strcmp(msg, "unknown command")) {
        int64_t now = esp_timer_get_time();
        if (now - s_huh_at > 8000000) { s_huh_at = now; fx_say(SND_HUH); }   // never "huh?" at its own "huh?"
        strlcpy(msg, "didn't understand", sizeof msg);
    }
    strlcpy(why, tag, sizeof why);                 // tag + msg, cut to fit (the note only shows 48 chars)
    strlcat(why, msg, sizeof why);
    note(t, pr, why);
    ESP_LOGI(TAG, "%s\"%s\" -> %s", tag, t, msg);
}

// mic -> AFE. Never blocks on anything but the I2S read.
static void feed_task(void *arg)
{
    int n = s_afe->get_feed_chunksize(s_afed) * s_afe->get_feed_channel_num(s_afed);
    int16_t *buf = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    for (;;) {
        if (esp_codec_dev_read(s_in, buf, n * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            strlcpy(s_status, "mic read failing", sizeof s_status);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        // level meter on the raw mic (channel 0 of the interleaved frame): it's what the gain slider moves.
        // RMS in dBFS, -60..0 dB -> 0..100, peak hold with a slow fall
        int ch = s_afe->get_feed_channel_num(s_afed), m = n / ch;
        double acc = 0;
        for (int i = 0; i < m; i++) acc += (double)buf[i * ch] * buf[i * ch];
        float db = 10.0f * log10f((float)(acc / (m ? m : 1)) / (32768.0f * 32768.0f) + 1e-9f);
        float lv = (db + 60.0f) * (100.0f / 60.0f);
        s_peak = lv > s_peak ? lv : s_peak * 0.93f;
        s_level = s_peak < 0 ? 0 : s_peak > 100 ? 100 : (int)s_peak;
        s_afe->feed(s_afed, buf);
    }
}

// AFE -> MultiNet. The AFE fetch size and MultiNet chunk size usually match (512); if not, re-chunk.
static void detect_task(void *arg)
{
    int chunk = s_mn->get_samp_chunksize(s_mnd);
    int16_t *mbuf = heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    int fill = 0;
    for (;;) {
        afe_fetch_result_t *r = s_afe->fetch_with_delay(s_afed, portMAX_DELAY);
        if (!r || r->ret_value == ESP_FAIL || !r->data) continue;
        if (r->vad_state == VAD_SPEECH) s_speech_us = esp_timer_get_time();
        fx_listening(s_wake && window_open(esp_timer_get_time()));
        int n = r->data_size / (int)sizeof(int16_t);
        bool cloud = s_seg && cloud_ready();


        if (s_reload) { s_reload = false; load_commands(); }

        for (int off = 0; off < n; ) {
            int take = chunk - fill < n - off ? chunk - fill : n - off;
            memcpy(mbuf + fill, r->data + off, take * sizeof(int16_t));
            fill += take; off += take;
            if (fill < chunk) break;
            fill = 0;

            esp_mn_state_t st = s_mn->detect(s_mnd, mbuf);
            if (st == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *res = s_mn->get_results(s_mnd);
                int id = res->command_id[0];
                if (id >= 1 && id <= MAX_PHRASES) {
                    const char *p = s_phr[id - 1];
                    float pr = res->prob[0], th = s_thresh / 100.0f;
                    int64_t now = esp_timer_get_time();
                    bool speech = now - s_speech_us < (int64_t)CONFIG_XARM_VOICE_VAD_HOLD_MS * 1000;
                    bool self = now < audio_play_end_us() + 250000;
                    // The gates only bite on weak matches: a confident one goes through even if the VAD
                    // missed it, or while Muffin is talking (the AEC has taken Muffin out of the mic).
                    const char *why = NULL;
                    if (!speech && pr < (th + 0.30f > 0.6f ? th + 0.30f : 0.6f)) why = "no voice heard";
                    else if (self && !is_stop(p) && pr < (th + 0.15f > 0.45f ? th + 0.15f : 0.45f)) why = "Muffin was talking";
                    else if (s_wake && !is_stop(p) && !is_wake(p) && !window_open(now)) why = "asleep - say Hey Muffin";
                    if (why) {
                        note(p, pr, why);
                        ESP_LOGI(TAG, "ignored \"%s\" (p=%.2f): %s", p, pr, why);
                    } else if (cloud && !s_seg_on && !is_stop(p) && now - s_sent_at < 1500000) {
                        note(p, pr, "→ cloud");          // MultiNet caught up after the clip already went out
                    } else if (cloud && s_seg_on && !is_stop(p)) {
                        // the cloud gets this utterance: the wake word just opens the window (and marks it for
                        // sending), anything else is kept as the fallback if the cloud call fails
                        if (is_wake(p)) { voice_engage(); s_seg_wake = true; fx_event(FX_HEARD); note(p, pr, "wake — listening ☁"); }
                        else { if (pr > s_seg_fbp) { strlcpy(s_seg_fb, p, sizeof s_seg_fb); s_seg_fbp = pr; } note(p, pr, "→ cloud"); }
                        ESP_LOGI(TAG, "\"%s\" (p=%.2f) -> cloud", p, pr);
                    } else {
                        if (is_stop(p)) s_seg_stopped = s_seg_on;
                        fx_event(FX_HEARD);
                        if (is_stop(p)) {
                            char msg[64];
                            strlcpy(s_last, p, sizeof s_last);
                            s_prob = pr;
                            router_exec(p, msg, sizeof msg);
                            note(p, pr, msg);
                            ESP_LOGI(TAG, "heard \"%s\" (p=%.2f) -> %s", p, pr, msg);
                        } else run_text(p, pr, "");     // "hey muffin" opens the window, commands keep it open
                    }
                }
                s_mn->clean(s_mnd);
            } else if (st == ESP_MN_STATE_TIMEOUT) {
                s_mn->clean(s_mnd);
            }
        }
        if (cloud) seg_feed(r->data, n, r->vad_state == VAD_SPEECH, esp_timer_get_time());
        else s_seg_on = false;
    }
}

static bool afe_start(srmodel_list_t *models)
{
    const char *fmt = audio_in_format();
    afe_config_t *c = afe_config_init(fmt, models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!c) return false;
    c->aec_init = true;                  // reference channel is always fed
    c->ns_init = false;                  // Espressif's advice for ASR: NS smears phonemes, AGC+AEC do the work
    c->vad_init = true;
    // VADNet (trained on ~15k h of speech) tells voices from servo whine and bells far better than WebRTC.
    // afe_config_init picks it when it's in srmodels.bin (CONFIG_SR_VADN_VADNET1_MEDIUM); a board whose
    // flash still has the old speech models gets WebRTC. Not assigned here: the config owns that string.
    if (!c->vad_model_name && esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL))
        ESP_LOGW(TAG, "VADNet is in flash but the AFE didn't pick it");
    c->vad_mode = VAD_MODE_1;            // lenient: the VAD only has to notice a voice, the matcher decides
    c->vad_min_speech_ms = 96;           // short commands ("stop") still register
    c->vad_min_noise_ms = 500;           // also how long a pause ends an utterance for the cloud ears
    c->wakenet_init = false;
    c->agc_init = true;
    c->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    s_afe = esp_afe_handle_from_config(c);
    s_afed = s_afe ? s_afe->create_from_config(c) : NULL;
    strlcpy(s_vad, c->vad_model_name ? "VADNet" : "WebRTC", sizeof s_vad);
    afe_config_free(c);
    if (!s_afed) return false;
    if (s_afe->get_feed_channel_num(s_afed) != audio_in_channels())
        ESP_LOGW(TAG, "AFE wants %d channels, mic gives %d", s_afe->get_feed_channel_num(s_afed), audio_in_channels());
    s_afe->print_pipeline(s_afed);
    ESP_LOGI(TAG, "AFE \"%s\": feed %d, fetch %d", fmt, s_afe->get_feed_chunksize(s_afed), s_afe->get_fetch_chunksize(s_afed));
    return true;
}

void voice_start(i2c_master_bus_handle_t bus)
{
    tune_load();
    s_in = audio_in();
    if (!s_in) { strlcpy(s_status, "no mic — audio codec didn't start", sizeof s_status); ESP_LOGE(TAG, "%s", s_status); return; }
    srmodel_list_t *models = esp_srmodel_init("model");
    char *name = models ? esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH) : NULL;
    if (!name) { strlcpy(s_status, "no English speech model in flash", sizeof s_status); ESP_LOGE(TAG, "%s", s_status); return; }
    if (!afe_start(models)) { strlcpy(s_status, "audio front-end failed to start", sizeof s_status); ESP_LOGE(TAG, "%s", s_status); return; }
    s_mn = esp_mn_handle_from_name(name);
    s_mnd = s_mn->create(name, 3000);
    if (!s_mnd) { strlcpy(s_status, "speech model failed to load", sizeof s_status); return; }
    // the command list must be set up before clear/add — without this every phrase is silently dropped
    esp_err_t e = esp_mn_commands_alloc(s_mn, s_mnd);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "commands_alloc: %s", esp_err_to_name(e));
    tune_apply();
    load_commands();
    s_seg = heap_caps_malloc(SEG_MAX * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_pre = heap_caps_calloc(PRE_N, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_pre) { free(s_seg); s_seg = NULL; }
    cloud_init();
    s_ok = true;
    ESP_LOGI(TAG, "listening (%s, %s VAD, %s), gain %d dB, threshold %d%%, %s", name, s_vad,
             audio_mic_dual() ? "both mics" : "one mic", s_gain, s_thresh, s_wake ? "Hey Muffin first" : "always on");
    xTaskCreatePinnedToCore(feed_task, "voice_feed", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(detect_task, "voice", 8192, NULL, 5, NULL, 1);
}

void voice_reload(void) { s_reload = true; }
bool voice_ok(void) { return s_ok; }
void voice_last(char *out, size_t n) { strlcpy(out, s_last, n); }
void voice_diag(char *status, size_t ns, int *level, float *prob, int *gain, int *thresh)
{
    strlcpy(status, s_status, ns);
    *level = s_level; *prob = s_prob; *gain = s_gain; *thresh = s_thresh;
}
void voice_diag2(bool *wake, bool *open, const char **vad)
{
    *wake = s_wake;
    *open = !s_wake || window_open(esp_timer_get_time());
    *vad = s_vad;
}
