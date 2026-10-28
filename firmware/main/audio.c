// Ported from xiaozhi-esp32 codecs/box_audio_codec.cc + boards/waveshare/esp32-s3-audio-board.
#include "audio.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev_defaults.h"
#include "sdkconfig.h"

#define PIN_MCLK 12
#define PIN_BCLK 13
#define PIN_WS   14
#define PIN_DIN  15
#define PIN_DOUT 16
#define TCA9555_ADDR 0x20      // EXIO8 (port 1 bit 0) = speaker PA enable

static const char *TAG = "audio";
static esp_codec_dev_handle_t s_in, s_out;

// TCA9555 port 1 bit 0 -> output high. Read-modify-write so buttons on the other pins keep working.
static void pa_enable(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t d = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TCA9555_ADDR, .scl_speed_hz = 100000 };
    i2c_master_dev_handle_t h;
    if (i2c_master_bus_add_device(bus, &d, &h) != ESP_OK) return;
    uint8_t reg, v;
    reg = 0x03; if (i2c_master_transmit_receive(h, &reg, 1, &v, 1, 50) == ESP_OK) {      // output port 1
        uint8_t w[2] = {0x03, (uint8_t)(v | 0x01)}; i2c_master_transmit(h, w, 2, 50); }
    reg = 0x07; if (i2c_master_transmit_receive(h, &reg, 1, &v, 1, 50) == ESP_OK) {      // config port 1 (1 = input)
        uint8_t w[2] = {0x07, (uint8_t)(v & ~0x01)}; i2c_master_transmit(h, w, 2, 50); }
    i2c_master_bus_rm_device(h);
}

bool audio_init(i2c_master_bus_handle_t bus)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear_after_cb = true;
    if (i2s_new_channel(&cc, &tx, &rx) != ESP_OK) return false;

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = PIN_DOUT, .din = I2S_GPIO_UNUSED },
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    i2s_tdm_config_t tdm = {
        .clk_cfg = { .sample_rate_hz = AUDIO_RATE, .clk_src = I2S_CLK_SRC_DEFAULT,
                     .mclk_multiple = I2S_MCLK_MULTIPLE_256, .bclk_div = 8 },
        .slot_cfg = { .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                      .slot_mode = I2S_SLOT_MODE_STEREO,
                      .slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3,
                      .ws_width = I2S_TDM_AUTO_WS_WIDTH, .bit_shift = true, .total_slot = I2S_TDM_AUTO_SLOT_NUM },
        .gpio_cfg = { .mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = I2S_GPIO_UNUSED, .din = PIN_DIN },
    };
    if (i2s_channel_init_std_mode(tx, &std) != ESP_OK || i2s_channel_init_tdm_mode(rx, &tdm) != ESP_OK
        || i2s_channel_enable(tx) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) {
        ESP_LOGE(TAG, "i2s init failed");
        return false;
    }

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    // speaker: ES8311
    audio_codec_i2c_cfg_t out_i2c = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    es8311_codec_cfg_t es8311 = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&out_i2c), .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC, .pa_pin = -1, .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    esp_codec_dev_cfg_t oc = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es8311_codec_new(&es8311), .data_if = data_if };
    s_out = esp_codec_dev_new(&oc);
    esp_codec_dev_sample_info_t ofs = { .bits_per_sample = 16, .channel = 1, .sample_rate = AUDIO_RATE };
    if (!s_out || esp_codec_dev_open(s_out, &ofs) != ESP_CODEC_DEV_OK) { ESP_LOGW(TAG, "speaker init failed"); s_out = NULL; }
    else { esp_codec_dev_set_out_vol(s_out, CONFIG_XARM_VOLUME); pa_enable(bus); }

    // mics: ES7210, channel 0 only
    audio_codec_i2c_cfg_t in_i2c = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    es7210_codec_cfg_t es7210 = { .ctrl_if = audio_codec_new_i2c_ctrl(&in_i2c),
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4 };
    esp_codec_dev_cfg_t ic = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = es7210_codec_new(&es7210), .data_if = data_if };
    s_in = esp_codec_dev_new(&ic);
    esp_codec_dev_sample_info_t ifs = { .bits_per_sample = 16, .channel = 4,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), .sample_rate = AUDIO_RATE };
    if (!s_in || esp_codec_dev_open(s_in, &ifs) != ESP_CODEC_DEV_OK) { ESP_LOGW(TAG, "mic init failed"); s_in = NULL; }
    else esp_codec_dev_set_in_channel_gain(s_in, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), CONFIG_XARM_MIC_GAIN_DB);

    ESP_LOGI(TAG, "speaker %s, mic %s", s_out ? "ok" : "off", s_in ? "ok" : "off");
    return s_in || s_out;
}

esp_codec_dev_handle_t audio_in(void) { return s_in; }

bool audio_play(const int16_t *pcm, size_t samples)
{
    return s_out && esp_codec_dev_write(s_out, (void *)pcm, samples * sizeof(int16_t)) == ESP_CODEC_DEV_OK;
}

void audio_volume(int pct) { if (s_out) esp_codec_dev_set_out_vol(s_out, pct); }
