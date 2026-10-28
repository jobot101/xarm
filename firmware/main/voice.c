// Mic path ported from xiaozhi-esp32 boards/waveshare/esp32-s3-audio-board + codecs/box_audio_codec.cc
// (input only, 16 kHz, mic channel 0). MultiNet runs continuously — no wake word — so "stop" is instant.
#include "voice.h"
#include <string.h>
#include "router.h"
#include "audio.h"
#include "fx.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define RATE 16000
#define MAX_PHRASES 24

static const char *TAG = "voice";
static esp_codec_dev_handle_t s_in;
static const esp_mn_iface_t *s_mn;
static model_iface_data_t *s_mnd;
static char s_phr[MAX_PHRASES][48];
static volatile bool s_ok, s_reload;
static char s_last[48] = "";

static void load_commands(void)
{
    int n = router_phrases(s_phr, MAX_PHRASES);
    esp_mn_commands_clear();
    for (int i = 0; i < n; i++) esp_mn_commands_add(i + 1, s_phr[i]);
    esp_mn_error_t *err = esp_mn_commands_update();
    if (err && err->num) ESP_LOGW(TAG, "%d phrases rejected by MultiNet", err->num);
    s_mn->print_active_speech_commands(s_mnd);
}

static void task(void *arg)
{
    int chunk = s_mn->get_samp_chunksize(s_mnd);        // samples per detect() call
    int16_t *buf = heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    for (;;) {
        if (s_reload) { s_reload = false; load_commands(); }
        if (esp_codec_dev_read(s_in, buf, chunk * sizeof(int16_t)) != ESP_CODEC_DEV_OK) continue;
        esp_mn_state_t st = s_mn->detect(s_mnd, buf);
        if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = s_mn->get_results(s_mnd);
            int id = r->command_id[0];
            if (id >= 1 && id <= MAX_PHRASES) {
                strlcpy(s_last, s_phr[id - 1], sizeof s_last);
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
    s_in = audio_in();
    if (!s_in) { ESP_LOGE(TAG, "no mic — voice off"); return; }
    srmodel_list_t *models = esp_srmodel_init("model");
    char *name = models ? esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH) : NULL;
    if (!name) { ESP_LOGE(TAG, "no English MultiNet model in 'model' partition — voice off"); return; }
    s_mn = esp_mn_handle_from_name(name);
    s_mnd = s_mn->create(name, 3000);
    s_mn->set_det_threshold(s_mnd, CONFIG_XARM_VOICE_THRESHOLD / 100.0f);
    load_commands();
    s_ok = true;
    ESP_LOGI(TAG, "listening (%s)", name);
    xTaskCreatePinnedToCore(task, "voice", 8192, NULL, 5, NULL, 1);
}

void voice_reload(void) { s_reload = true; }
bool voice_ok(void) { return s_ok; }
void voice_last(char *out, size_t n) { strlcpy(out, s_last, n); }
