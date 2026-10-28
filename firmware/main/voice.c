// Mic path ported from xiaozhi-esp32 boards/waveshare/esp32-s3-audio-board + codecs/box_audio_codec.cc
// (input only, 16 kHz, mic channel 0). MultiNet runs continuously — no wake word — so "stop" is instant.
#include "voice.h"
#include <string.h>
#include "router.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define RATE 16000
#define PIN_MCLK 12
#define PIN_BCLK 13
#define PIN_WS   14
#define PIN_DIN  15
#define MAX_PHRASES 24

static const char *TAG = "voice";
static esp_codec_dev_handle_t s_in;
static const esp_mn_iface_t *s_mn;
static model_iface_data_t *s_mnd;
static char s_phr[MAX_PHRASES][48];
static volatile bool s_ok, s_reload;
static char s_last[48] = "";

static bool mic_init(i2c_master_bus_handle_t bus)
{
    i2s_chan_handle_t rx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cc, NULL, &rx) != ESP_OK) return false;
    i2s_tdm_config_t tdm = {
        .clk_cfg = { .sample_rate_hz = RATE, .clk_src = I2S_CLK_SRC_DEFAULT,
                     .mclk_multiple = I2S_MCLK_MULTIPLE_256, .bclk_div = 8 },
        .slot_cfg = { .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                      .slot_mode = I2S_SLOT_MODE_STEREO,
                      .slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3,
                      .ws_width = I2S_TDM_AUTO_WS_WIDTH, .bit_shift = true, .total_slot = I2S_TDM_AUTO_SLOT_NUM },
        .gpio_cfg = { .mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS,
                      .dout = I2S_GPIO_UNUSED, .din = PIN_DIN },
    };
    if (i2s_channel_init_tdm_mode(rx, &tdm) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) return false;

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    es7210_codec_cfg_t es = { .ctrl_if = ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4 };
    const audio_codec_if_t *codec = es7210_codec_new(&es);
    esp_codec_dev_cfg_t dc = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    s_in = esp_codec_dev_new(&dc);
    if (!s_in) return false;

    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 4,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), .sample_rate = RATE };
    if (esp_codec_dev_open(s_in, &fs) != ESP_CODEC_DEV_OK) return false;
    esp_codec_dev_set_in_channel_gain(s_in, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), CONFIG_XARM_MIC_GAIN_DB);
    return true;
}

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
    if (!mic_init(bus)) { ESP_LOGE(TAG, "mic init failed — voice off"); return; }
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
