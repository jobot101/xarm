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

// ---------------------------------------------------------------- palette: stained glass
typedef struct { float r, g, b; } rgb_t;
static const rgb_t RUBY = {0.70f, 0.04f, 0.12f}, SAPPHIRE = {0.10f, 0.22f, 0.80f}, EMERALD = {0.05f, 0.55f, 0.25f},
                   GOLD = {0.95f, 0.68f, 0.18f}, AMETHYST = {0.45f, 0.10f, 0.60f};
static const rgb_t ROSE[LED_N] = {RUBY, GOLD, SAPPHIRE, EMERALD, RUBY, AMETHYST, SAPPHIRE};
static const rgb_t JEWEL[4] = {RUBY, SAPPHIRE, EMERALD, GOLD};

// ---------------------------------------------------------------- sound: bells + organ
// a sound is a list of notes: voice, frequency, length, volume (freq 0 = rest)
enum { ORGAN, BELL };
typedef struct { uint8_t voice; uint16_t f, ms; uint8_t vol; } note_t;
typedef enum { S_BOOT, S_HEARD, S_THINK, S_MOVE, S_GRAB, S_HAPPY, S_DONE, S_SAD, S_STOP,
               S_STRIKE, S_CHEER, S_WAVE, S_BOW, S_DANCE, S_CHIRP, S_FANFARE, S_RUMBLE, S_GREET,
               S_SHOW0, S_COUNT = S_SHOW0 + 4 } snd_t;

#define C4 262
#define D4 294
#define E4 330
#define F4 349
#define G4 392
#define A4 440
#define C5 523
#define D5 587
#define E5 659
#define G5 784
#define A5 880
#define C6 1047
#define N(...) (note_t[]){__VA_ARGS__}
static const struct { const note_t *n; int len; } SOUNDS[S_COUNT] = {
    [S_BOOT]   = { N({ORGAN,C4,150,45},{ORGAN,E4,150,45},{ORGAN,G4,150,45},{BELL,C5,1100,70}), 4 },
    [S_HEARD]  = { N({BELL,A5,380,40}), 1 },
    [S_MOVE]   = { N({ORGAN,G4,90,30},{ORGAN,C5,120,30}), 2 },
    [S_GRAB]   = { N({BELL,C4,320,60},{ORGAN,E5,110,35}), 2 },
    [S_HAPPY]  = { N({BELL,E5,320,55},{BELL,G5,600,55}), 2 },
    [S_DONE]   = { N({ORGAN,C5,80,40},{ORGAN,G5,80,40},{ORGAN,C6,200,40}), 3 },
    [S_SAD]    = { N({ORGAN,A4,200,45},{ORGAN,F4,200,45},{ORGAN,D4,380,45}), 3 },
    [S_STOP]   = { N({BELL,131,900,85}), 1 },
    [S_STRIKE] = { N({BELL,C5,450,80}), 1 },
    [S_CHEER]  = { N({ORGAN,C5,100,45},{ORGAN,E5,100,45},{ORGAN,G5,100,45},{BELL,C6,700,60}), 4 },
    [S_WAVE]   = { N({BELL,A5,260,45},{BELL,E5,500,45}), 2 },
    [S_BOW]    = { N({BELL,G5,350,50},{BELL,C5,800,55}), 2 },
    [S_DANCE]  = { N({ORGAN,C5,160,40},{ORGAN,E5,160,40},{ORGAN,G5,160,40},{ORGAN,E5,160,40},
                     {ORGAN,A5,160,40},{ORGAN,G5,160,40},{ORGAN,E5,160,40},{BELL,C5,500,50}), 8 },
    [S_FANFARE] = { N({ORGAN,G4,130,55},{ORGAN,C5,130,55},{ORGAN,E5,130,55},{ORGAN,G5,380,60},
                      {ORGAN,E5,130,55},{ORGAN,G5,500,60},{BELL,C6,900,70}), 7 },
    [S_SHOW0]     = { N({BELL,C5,420,60}), 1 },
    [S_SHOW0 + 1] = { N({BELL,E5,420,60}), 1 },
    [S_SHOW0 + 2] = { N({BELL,G5,420,60}), 1 },
    [S_SHOW0 + 3] = { N({BELL,C6,420,60}), 1 },
    [S_THINK] = { NULL, 0 },   // random organ noodling, generated on the fly
    [S_CHIRP] = { NULL, 0 },   // soft blip while working
    [S_RUMBLE] = { NULL, 0 },  // recorded robot voice: "Ready to rumble!" (tools/make_voice.py)
    [S_GREET] = { NULL, 0 },   // per-kid jingle, built from s_greet_seed
};

static QueueHandle_t s_sq;
static volatile unsigned s_greet_seed;
extern const uint8_t rumble_start[] asm("_binary_voice_rumble_pcm_start");
extern const uint8_t rumble_end[]   asm("_binary_voice_rumble_pcm_end");

// church-bell partials (hum, prime, tierce, quint, nominal, ...) with per-partial decay
static const float BP_RATIO[6] = {0.5f, 1.0f, 1.19f, 1.5f, 2.0f, 2.66f};
static const float BP_AMP[6]   = {0.45f, 1.0f, 0.55f, 0.35f, 0.40f, 0.20f};
static const float BP_TAU[6]   = {0.9f, 0.55f, 0.40f, 0.30f, 0.25f, 0.15f};   // seconds

static void synth(const note_t *n, int count)
{
    static int16_t buf[256];
    int k = 0;
    for (int i = 0; i < count; i++) {
        int len = n[i].ms * AUDIO_RATE / 1000;
        float amp = n[i].vol / 100.0f * 9000.0f, ph[6] = {0};
        for (int s = 0; s < len; s++) {
            float v = 0, t = (float)s / AUDIO_RATE;
            if (n[i].f && n[i].voice == BELL) {
                for (int p = 0; p < 6; p++) {
                    ph[p] += 6.2832f * n[i].f * BP_RATIO[p] / AUDIO_RATE;
                    if (ph[p] > 6.2832f) ph[p] -= 6.2832f;
                    v += BP_AMP[p] * expf(-t / BP_TAU[p]) * sinf(ph[p]);
                }
                v *= 0.42f * (s < 40 ? s / 40.0f : 1.0f);                       // soft strike
                if (len - s < 160) v *= (len - s) / 160.0f;
            } else if (n[i].f) {                                                // organ: drawbars 8' 4' 2 2/3' 2'
                ph[0] += 6.2832f * n[i].f / AUDIO_RATE;
                if (ph[0] > 6.2832f) ph[0] -= 6.2832f;
                v = sinf(ph[0]) + 0.5f * sinf(2 * ph[0]) + 0.25f * sinf(3 * ph[0]) + 0.15f * sinf(4 * ph[0]);
                v *= 0.55f;
                int atk = AUDIO_RATE / 70, rel = AUDIO_RATE / 40;
                v *= s < atk ? (float)s / atk : (len - s < rel ? (float)(len - s) / rel : 1.0f);
            }
            buf[k++] = (int16_t)(v * amp);
            if (k == 256) { audio_play(buf, k); k = 0; }
        }
    }
    if (k) audio_play(buf, k);
}

static int rnd(int lo, int hi) { return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1)); }

static void sound_task(void *arg)
{
    static const uint16_t PENTA[6] = {C5, D5, E5, G5, A5, C6};
    snd_t s;
    note_t r[8];
    for (;;) {
        xQueueReceive(s_sq, &s, portMAX_DELAY);
        if (s == S_THINK) {
            for (int i = 0; i < 6; i++) r[i] = (note_t){ORGAN, PENTA[rnd(0, 5)], 90, 28};
            synth(r, 6);
        } else if (s == S_RUMBLE) {
            audio_play((const int16_t *)rumble_start, (rumble_end - rumble_start) / 2);
        } else if (s == S_GREET) {
            unsigned h = s_greet_seed;
            for (int i = 0; i < 3; i++) { r[i] = (note_t){BELL, PENTA[h % 6], (uint16_t)(i == 2 ? 700 : 240), 55}; h /= 6; }
            synth(r, 3);
        } else if (s == S_CHIRP) {
            r[0] = (note_t){ORGAN, PENTA[rnd(0, 5)], 60, 14};
            synth(r, 1);
        } else {
            synth(SOUNDS[s].n, SOUNDS[s].len);
        }
    }
}

static void play(snd_t s) { if (s_sq) xQueueSend(s_sq, &s, 0); }

// ---------------------------------------------------------------- lights
typedef enum { L_BOOT, L_IDLE, L_THINK, L_WORK, L_HOLD, L_WARN, L_STOP, L_UPDATE, L_PARTY } led_mode_t;

static led_strip_handle_t s_led;
static volatile led_mode_t s_mode = L_BOOT;
static volatile int s_flash, s_show_t, s_show = -1;
static volatile bool s_working;

static void px(int i, rgb_t c, float k)
{
    led_strip_set_pixel(s_led, i % LED_N, (uint8_t)(c.r * k * 255 * BRIGHT), (uint8_t)(c.g * k * 255 * BRIGHT),
                        (uint8_t)(c.b * k * 255 * BRIGHT));
}

static void fill(rgb_t c, float k) { for (int i = 0; i < LED_N; i++) px(i, c, k); }

// one lit head with a fading tail over a dim background
static void spinner(int t, rgb_t c, rgb_t bg, int speed)
{
    int head = (t * speed / 8) % LED_N;
    for (int i = 0; i < LED_N; i++) {
        int d = (head - i + LED_N) % LED_N;
        if (d == 0) px(i, c, 1.0f);
        else if (d == 1) px(i, c, 0.35f);
        else px(i, bg, 0.08f);
    }
}

static void led_task(void *arg)
{
    int t = 0, since_chirp = 0;
    for (;; t++) {
        float breathe = 0.5f - 0.5f * cosf(t * 0.07f);     // ~3 s
        float pulse = 0.5f - 0.5f * cosf(t * 0.25f);
        switch (s_mode) {
        case L_BOOT:                                         // jewels light one by one
            for (int i = 0; i < LED_N; i++) px(i, ROSE[i], i <= t / 6 ? 0.8f : 0.0f);
            if (t > 6 * LED_N + 30) s_mode = L_IDLE;
            break;
        case L_IDLE:                                         // rose window: sunlight drifting through the glass
            for (int i = 0; i < LED_N; i++)
                px(i, ROSE[(i + t / 90) % LED_N], 0.10f + 0.22f * (0.5f - 0.5f * cosf(t * 0.05f + i * 0.9f)));
            break;
        case L_THINK:  spinner(t, SAPPHIRE, SAPPHIRE, 3); break;
        case L_WORK:   spinner(t, GOLD, RUBY, 4); break;
        case L_HOLD:   fill(EMERALD, 0.45f + 0.5f * breathe); break;
        case L_WARN:   fill(GOLD, 0.15f + 0.85f * pulse); break;
        case L_STOP:   fill(RUBY, (t / 6) % 2 ? 1.0f : 0.15f); break;
        case L_UPDATE: spinner(t, AMETHYST, SAPPHIRE, 6); break;
        case L_PARTY:  for (int i = 0; i < LED_N; i++) px(i, ROSE[(i + t / 3) % LED_N], 1.0f); break;
        }
        if (s_show_t > 0) { fill(JEWEL[s_show & 3], 1.0f); s_show_t--; }
        if (s_flash > 0) { rgb_t w = {1, 1, 1}; fill(w, s_flash / 10.0f); s_flash--; }
        led_strip_refresh(s_led);

        // soft organ blips while it works, so you can hear it thinking
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
    s_sq = xQueueCreate(6, sizeof(snd_t));
    xTaskCreate(sound_task, "fx_snd", 4096, NULL, 4, NULL);
}

static bool is(const char *a, const char *b) { return !strcmp(a, b); }

void fx_state(const char *st)
{
    led_mode_t was = s_mode;
    s_working = false;
    if      (is(st, "searching"))                                   { s_mode = L_THINK;  play(S_THINK); s_working = true; }
    else if (is(st, "moving") || is(st, "dropping") || is(st, "sorting") || is(st, "giving") || is(st, "stacking"))
                                                                    { s_mode = L_WORK;   play(S_MOVE);  s_working = true; }
    else if (is(st, "grasping"))                                    { s_mode = L_WORK;   play(S_GRAB);  s_working = true; }
    else if (is(st, "holding"))                                     { s_mode = L_HOLD;   play(S_HAPPY); }
    else if (is(st, "waving"))                                      { s_mode = L_PARTY;  play(S_WAVE); }
    else if (is(st, "bowing"))                                      { s_mode = L_PARTY;  play(S_BOW); }
    else if (is(st, "dancing"))                                     { s_mode = L_PARTY;  play(S_DANCE); play(S_DANCE); }
    else if (is(st, "helping"))                                     { s_mode = L_HOLD; }
    else if (is(st, "ringing") || is(st, "playing"))                { s_mode = L_PARTY;  s_working = is(st, "playing"); }
    else if (is(st, "not_found") || is(st, "need_calib"))           { s_mode = L_WARN;   play(S_SAD); }
    else if (is(st, "stopped"))                                     { s_mode = L_STOP;   play(S_STOP); }
    else if (is(st, "updating"))                                    { s_mode = L_UPDATE; play(S_THINK); }
    else if (is(st, "idle"))                                        { if (was == L_WORK) play(S_DONE); s_mode = L_IDLE; }
}

void fx_event(fx_event_t e)
{
    switch (e) {
    case FX_BOOT:  s_mode = L_BOOT; play(S_BOOT); break;
    case FX_HEARD: s_flash = 10; play(S_HEARD); break;
    case FX_ERROR: s_mode = L_WARN; play(S_SAD); break;
    case FX_BELL:  play(S_STRIKE); break;
    case FX_CHEER: s_mode = L_PARTY; play(S_CHEER); break;
    case FX_UPDATED: s_mode = L_PARTY; play(S_FANFARE); play(S_RUMBLE); break;
    }
}

void fx_show(int j)
{
    s_show = j & 3;
    s_show_t = 14;            // ~0.45 s
    play((snd_t)(S_SHOW0 + (j & 3)));
}

void fx_greet(int jewel, unsigned seed)
{
    s_greet_seed = seed;
    s_show = jewel & 3;
    s_show_t = 60;            // ~2 s in their color
    play(S_GREET);
}
