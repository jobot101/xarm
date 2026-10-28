// Mic path ported from xiaozhi-esp32 boards/waveshare/esp32-s3-audio-board + codecs/box_audio_codec.cc
// (16 kHz, mic channel 0). MultiNet runs continuously — no wake word — so "stop" is instant.
#include "voice.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "router.h"
#include "audio.h"
#include "fx.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_codec_dev.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define MAX_PHRASES 64

static const char *TAG = "voice";
static esp_codec_dev_handle_t s_in;
static const esp_mn_iface_t *s_mn;
static model_iface_data_t *s_mnd;
static char s_phr[MAX_PHRASES][48];
static volatile bool s_ok, s_reload;
static char s_last[48] = "", s_status[64] = "starting";
static volatile int s_level;          // mic level 0..100 (dBFS mapped, peak-hold)
static volatile float s_prob;         // confidence of the last recognized phrase
static int s_gain = CONFIG_XARM_MIC_GAIN_DB, s_thresh = CONFIG_XARM_VOICE_THRESHOLD;

// runtime tuning, saved in NVS so the app can adjust without a rebuild
static void tune_load(void)
{
    nvs_handle_t h;
    if (nvs_open("xarm", NVS_READONLY, &h) != ESP_OK) return;
    int32_t v;
    if (nvs_get_i32(h, "mic_gain", &v) == ESP_OK) s_gain = v;
    if (nvs_get_i32(h, "mn_thresh", &v) == ESP_OK) s_thresh = v;
    nvs_close(h);
}

static void tune_apply(void)
{
    if (s_in) esp_codec_dev_set_in_channel_gain(s_in, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), (float)s_gain);
    if (s_mn && s_mnd) s_mn->set_det_threshold(s_mnd, s_thresh / 100.0f);
}

void voice_tune(int gain_db, int thresh_pct)
{
    if (gain_db >= 0 && gain_db <= 42) s_gain = gain_db;
    if (thresh_pct >= 5 && thresh_pct <= 90) s_thresh = thresh_pct;
    tune_apply();
    nvs_handle_t h;
    if (nvs_open("xarm", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "mic_gain", s_gain);
        nvs_set_i32(h, "mn_thresh", s_thresh);
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

static void task(void *arg)
{
    int chunk = s_mn->get_samp_chunksize(s_mnd);        // samples per detect() call
    int16_t *buf = heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    float peak = 0;
    for (;;) {
        if (s_reload) { s_reload = false; load_commands(); }
        if (esp_codec_dev_read(s_in, buf, chunk * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            strlcpy(s_status, "mic read failing", sizeof s_status);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        // level meter: RMS in dBFS, -60..0 dB -> 0..100, with a slow fall
        double acc = 0;
        for (int i = 0; i < chunk; i++) acc += (double)buf[i] * buf[i];
        float db = 10.0f * log10f((float)(acc / chunk) / (32768.0f * 32768.0f) + 1e-9f);
        float lv = (db + 60.0f) * (100.0f / 60.0f);
        peak = lv > peak ? lv : peak * 0.93f;
        s_level = peak < 0 ? 0 : peak > 100 ? 100 : (int)peak;

        esp_mn_state_t st = s_mn->detect(s_mnd, buf);
        // the board's own bells/organ/voice come back through the mic and fake commands (often "stop",
        // which kills whatever trick is running) — ignore anything heard while the speaker plays + 400 ms
        bool self = esp_timer_get_time() < audio_play_end_us() + 400000;
        if (st == ESP_MN_STATE_DETECTED && self) {
            s_mn->clean(s_mnd);
        } else if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = s_mn->get_results(s_mnd);
            int id = r->command_id[0];
            if (id >= 1 && id <= MAX_PHRASES) {
                strlcpy(s_last, s_phr[id - 1], sizeof s_last);
                s_prob = r->prob[0];
                fx_event(FX_HEARD);
                char msg[64];
                router_exec(s_last, msg, sizeof msg);
                ESP_LOGI(TAG, "heard \"%s\" (p=%.2f) -> %s", s_last, r->prob[0], msg);
            }
            s_mn->clean(s_mnd);
        } else if (st == ESP_MN_STATE_TIMEOUT) {
            s_mn->clean(s_mnd);
        }
    }
}

void voice_start(i2c_master_bus_handle_t bus)
{
    tune_load();
    s_in = audio_in();
    if (!s_in) { strlcpy(s_status, "no mic — audio codec didn't start", sizeof s_status); ESP_LOGE(TAG, "%s", s_status); return; }
    srmodel_list_t *models = esp_srmodel_init("model");
    char *name = models ? esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH) : NULL;
    if (!name) { strlcpy(s_status, "no English speech model in flash", sizeof s_status); ESP_LOGE(TAG, "%s", s_status); return; }
    s_mn = esp_mn_handle_from_name(name);
    s_mnd = s_mn->create(name, 3000);
    if (!s_mnd) { strlcpy(s_status, "speech model failed to load", sizeof s_status); return; }
    // the command list must be set up before clear/add — without this every phrase is silently dropped
    esp_err_t e = esp_mn_commands_alloc(s_mn, s_mnd);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "commands_alloc: %s", esp_err_to_name(e));
    tune_apply();
    load_commands();
    s_ok = true;
    ESP_LOGI(TAG, "listening (%s), gain %d dB, threshold %d%%", name, s_gain, s_thresh);
    xTaskCreatePinnedToCore(task, "voice", 8192, NULL, 5, NULL, 1);
}

void voice_reload(void) { s_reload = true; }
bool voice_ok(void) { return s_ok; }
void voice_last(char *out, size_t n) { strlcpy(out, s_last, n); }
void voice_diag(char *status, size_t ns, int *level, float *prob, int *gain, int *thresh)
{
    strlcpy(status, s_status, ns);
    *level = s_level; *prob = s_prob; *gain = s_gain; *thresh = s_thresh;
}
