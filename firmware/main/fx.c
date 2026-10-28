#include "fx.h"
#include <math.h>
#include <string.h>
#include "audio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define LED_GPIO 38
#define LED_N    7            // Waveshare says 7 (xiaozhi drives 6) — an extra pixel is harmless
#define BRIGHT   0.30f        // cap: the ring runs off USB power

static const char *TAG = "fx";

// ---------------------------------------------------------------- sound
// a sound is a list of glides: frequency f0 -> f1 over ms, at vol (0 = rest)
typedef struct { uint16_t f0, f1, ms; uint8_t vol; } tone_t;
typedef enum { S_BOOT, S_BLIP, S_THINK, S_MOVE, S_GRAB, S_HAPPY, S_DONE, S_SAD, S_STOP, S_CHIRP, S_COUNT } snd_t;

#define T(...) (tone_t[]){__VA_ARGS__}
static const struct { const tone_t *t; int n; } SOUNDS[S_COUNT] = {
    [S_BOOT]  = { T({523,523,90,60},{0,0,30,0},{784,784,90,60},{0,0,30,0},{1047,1568,220,70}), 5 },
    [S_BLIP]  = { T({1800,2400,45,55},{0,0,25,0},{2400,2900,45,55}), 3 },
    [S_MOVE]  = { T({420,900,140,45},{900,700,60,35}), 2 },
    [S_GRAB]  = { T({180,140,90,70},{0,0,40,0},{600,1200,110,55}), 3 },
    [S_HAPPY] = { T({880,880,80,55},{0,0,20,0},{1175,1760,170,60}), 3 },
    [S_DONE]  = { T({1320,990,90,50},{0,0,20,0},{990,1480,120,50}), 3 },
    [S_SAD]   = { T({700,520,220,55},{520,330,320,50}), 2 },
    [S_STOP]  = { T({160,150,120,80},{0,0,40,0},{160,120,180,80}), 3 },
    [S_THINK] = { NULL, 0 },   // random R2-style warble, generated on the fly
    [S_CHIRP] = { NULL, 0 },   // tiny random blip while working
};

static QueueHandle_t s_sq;

static void synth(const tone_t *t, int n)
{
    static int16_t buf[256];
    float ph = 0;
    for (int i = 0; i < n; i++) {
        int len = t[i].ms * AUDIO_RATE / 1000, k = 0;
        int atk = AUDIO_RATE / 250, rel = AUDIO_RATE / 80;       // 4 ms attack, 12 ms release
        float amp = t[i].vol / 100.0f * 9000.0f;
        for (int s = 0; s < len; s++) {
            float f = t[i].f0 + (t[i].f1 - t[i].f0) * (float)s / len;
            f *= 1.0f + 0.012f * sinf(s * 6.2832f * 28.0f / AUDIO_RATE);   // a little servo-ish vibrato
            ph += 6.2832f * f / AUDIO_RATE;
            if (ph > 6.2832f) ph -= 6.2832f;
            float env = s < atk ? (float)s / atk : (len - s < rel ? (float)(len - s) / rel : 1.0f);
            // sine + odd harmonics = chiptune-robot timbre
            float v = t[i].vol ? sinf(ph) + 0.33f * sinf(3 * ph) + 0.14f * sinf(5 * ph) : 0;
            buf[k++] = (int16_t)(v * env * amp * 0.68f);
            if (k == 256) { audio_play(buf, k); k = 0; }
        }
        if (k) audio_play(buf, k);
    }
}

static int rnd(int lo, int hi) { return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1)); }

static void sound_task(void *arg)
{
    snd_t s;
    tone_t r[10];
    for (;;) {
        xQueueReceive(s_sq, &s, portMAX_DELAY);
        if (s == S_THINK) {
            int n = rnd(6, 9);
            for (int i = 0; i < n; i++) r[i] = (tone_t){ rnd(700, 2600), rnd(700, 2600), rnd(35, 80), rnd(30, 50) };
            synth(r, n);
        } else if (s == S_CHIRP) {
            r[0] = (tone_t){ rnd(1500, 2800), rnd(1500, 2800), rnd(25, 45), 18 };
            synth(r, 1);
        } else {
            synth(SOUNDS[s].t, SOUNDS[s].n);
        }
    }
}

static void play(snd_t s) { if (s_sq) xQueueSend(s_sq, &s, 0); }

// ---------------------------------------------------------------- lights
typedef enum { L_BOOT, L_IDLE, L_THINK, L_WORK, L_HOLD, L_WARN, L_STOP, L_UPDATE } led_mode_t;

static led_strip_handle_t s_led;
static volatile led_mode_t s_mode = L_BOOT;
static volatile int s_flash;                  // ticks left of a white "heard you" flash
static volatile bool s_working;

static void px(int i, float r, float g, float b)
{
    led_strip_set_pixel(s_led, i % LED_N, (uint8_t)(r * 255 * BRIGHT), (uint8_t)(g * 255 * BRIGHT), (uint8_t)(b * 255 * BRIGHT));
}

// one lit head with a fading tail, going around
static void spinner(int t, float r, float g, float b, int speed)
{
    int head = (t * speed / 8) % LED_N;
    for (int i = 0; i < LED_N; i++) {
        int d = (head - i + LED_N) % LED_N;
        float k = d == 0 ? 1.0f : d == 1 ? 0.35f : d == 2 ? 0.1f : 0.0f;
        px(i, r * k, g * k, b * k);
    }
}

static void fill(float r, float g, float b) { for (int i = 0; i < LED_N; i++) px(i, r, g, b); }

static void led_task(void *arg)
{
    int t = 0, since_chirp = 0;
    const float BR = 0.79f, BG = 0.64f, BB = 0.29f;      // brass
    for (;; t++) {
        float breathe = 0.5f - 0.5f * cosf(t * 0.07f);    // ~3 s
        float pulse = 0.5f - 0.5f * cosf(t * 0.25f);
        switch (s_mode) {
        case L_BOOT:   { float h = (float)(t % LED_N); for (int i = 0; i < LED_N; i++) { float k = (i == (int)h) ? 1 : 0.05f; px(i, BR * k, BG * k, BB * k); } if (t > 50) s_mode = L_IDLE; break; }
        case L_IDLE:   fill(BR * (0.06f + 0.18f * breathe), BG * (0.06f + 0.18f * breathe), BB * (0.06f + 0.18f * breathe)); break;
        case L_THINK:  spinner(t, 0.1f, 0.7f, 1.0f, 3); break;
        case L_WORK:   spinner(t, BR, BG, BB, 4); break;
        case L_HOLD:   fill(0.1f, 0.55f + 0.3f * breathe, 0.25f); break;
        case L_WARN:   fill(1.0f * pulse, 0.55f * pulse, 0.0f); break;
        case L_STOP:   fill((t / 6) % 2 ? 1.0f : 0.15f, 0.0f, 0.0f); break;
        case L_UPDATE: spinner(t, 0.2f, 0.3f, 1.0f, 6); break;
        }
        if (s_flash > 0) { float k = s_flash / 10.0f; fill(k, k, k); s_flash--; }
        led_strip_refresh(s_led);

        // little busy chirps while it's working, so you can hear it thinking
        if (s_working && ++since_chirp > rnd(25, 45)) { play(S_CHIRP); since_chirp = 0; }
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}

// ---------------------------------------------------------------- api
void fx_start(void)
{
    led_strip_config_t lc = {
        .strip_gpio_num = LED_GPIO, .max_leds = LED_N, .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rc = { .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&lc, &rc, &s_led) == ESP_OK) xTaskCreate(led_task, "fx_led", 3072, NULL, 2, NULL);
    else ESP_LOGW(TAG, "LED ring init failed");
    s_sq = xQueueCreate(4, sizeof(snd_t));
    xTaskCreate(sound_task, "fx_snd", 4096, NULL, 4, NULL);
}

void fx_state(const char *st)
{
    s_working = false;
    if      (!strcmp(st, "searching"))                          { s_mode = L_THINK;  play(S_THINK); s_working = true; }
    else if (!strcmp(st, "moving") || !strcmp(st, "dropping"))  { s_mode = L_WORK;   play(S_MOVE);  s_working = true; }
    else if (!strcmp(st, "grasping"))                           { s_mode = L_WORK;   play(S_GRAB);  s_working = true; }
    else if (!strcmp(st, "holding"))                            { s_mode = L_HOLD;   play(S_HAPPY); }
    else if (!strcmp(st, "not_found") || !strcmp(st, "need_calib")) { s_mode = L_WARN; play(S_SAD); }
    else if (!strcmp(st, "stopped"))                            { s_mode = L_STOP;   play(S_STOP); }
    else if (!strcmp(st, "updating"))                           { s_mode = L_UPDATE; play(S_THINK); }
    else if (!strcmp(st, "idle"))                               { if (s_mode == L_WORK) play(S_DONE); s_mode = L_IDLE; }
}

void fx_event(fx_event_t e)
{
    switch (e) {
    case FX_BOOT:  s_mode = L_BOOT; play(S_BOOT); break;
    case FX_HEARD: s_flash = 10; play(S_BLIP); break;
    case FX_ERROR: s_mode = L_WARN; play(S_SAD); break;
    }
}
