#include "fx.h"
#include <math.h>
#include <string.h>
#include "audio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "voice.h"
#include "sounds_gen.h"
#include "chat.h"
#include "sdkconfig.h"
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
// the rose window's four glass colors (ragamuffinstudios.com emblem), pushed a little for LEDs
static const rgb_t RUBY = {0.85f, 0.10f, 0.07f}, SAPPHIRE = {0.12f, 0.40f, 0.85f}, EMERALD = {0.30f, 0.72f, 0.10f},
                   GOLD = {0.95f, 0.62f, 0.05f}, AMETHYST = {0.45f, 0.10f, 0.60f};
static const rgb_t ROSE[LED_N] = {RUBY, GOLD, EMERALD, SAPPHIRE, RUBY, GOLD, EMERALD};   // petal order, clockwise
static const rgb_t JEWEL[4] = {RUBY, SAPPHIRE, EMERALD, GOLD};

// ---------------------------------------------------------------- sound: the bank
// Everything it says and plays is pre-rendered by tools/make_sounds.py (Piper voice, sampled-quality
// bells and organ in a hall, mastered for this speaker) into sounds.bin: IMA ADPCM clips, grouped.
// A group has variants; one is picked at random, never the same one twice in a row.
// The bank sits in the "model" flash partition after the speech models (FX_BANK_OFF), so it doesn't eat
// app space; update.c keeps it in sync over WiFi, and the phone flasher writes it on a full install.
typedef struct __attribute__((packed)) { char magic[4]; uint32_t layout, ngroups, nclips, data; } bank_hdr_t;
typedef struct __attribute__((packed)) { uint16_t first, n; } grp_t;
typedef struct __attribute__((packed)) { uint32_t off, n; int16_t first; uint8_t idx, khz; } clip_t;
static const uint8_t *s_bank;                 // mapped bank, NULL = silent
static const grp_t *s_grp;
static const clip_t *s_clip;
static uint32_t s_nclips;
static esp_partition_mmap_handle_t s_map;
static bool s_mapped;

typedef struct { uint16_t g; int8_t pick; uint8_t low; int64_t at; } req_t;
static QueueHandle_t s_sq;
static volatile bool s_hush;                  // stop: cut the current clip, drop the queue
static volatile bool s_talking;
static uint8_t s_lastpick[SND_GROUPS];
static volatile int64_t s_last_activity_us, s_last_said_us;

static int rnd(int lo, int hi) { return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1)); }

static const int16_t STEPS[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50,
    55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t IDX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

typedef struct { const uint8_t *p; uint32_t left, k; int pred, idx; } dec_t;   // k = nibble counter

static int dec_next(dec_t *d)
{
    uint8_t b = d->p[d->k >> 1];
    int c = d->k & 1 ? b >> 4 : b & 15, step = STEPS[d->idx], dq = step >> 3;
    d->k++;
    if (c & 4) dq += step;
    if (c & 2) dq += step >> 1;
    if (c & 1) dq += step >> 2;
    d->pred += c & 8 ? -dq : dq;
    if (d->pred > 32767) d->pred = 32767; else if (d->pred < -32768) d->pred = -32768;
    d->idx += IDX[c & 7];
    if (d->idx < 0) d->idx = 0; else if (d->idx > 88) d->idx = 88;
    return d->pred;
}

// decode one clip straight to the speaker; slower clips (speech at 11/12 kHz) are resampled to 16 kHz
// (linear) on the way
static void play_clip(uint32_t i)
{
    const clip_t *c = &s_clip[i];
    dec_t d = { s_bank + c->off, c->n, 0, c->first, c->idx };
    static int16_t buf[256];
    int k = 0;
    if (c->khz == AUDIO_RATE / 1000 || c->khz == 0) {
        buf[k++] = (int16_t)d.pred;
        for (uint32_t s = 1; s < c->n && !s_hush; s++) {
            buf[k++] = (int16_t)dec_next(&d);
            if (k == 256) { audio_play(buf, k); k = 0; }
        }
    } else {                                  // output j sits at input j * khz/16, phase in 1/16ths
        const uint32_t step = c->khz, one = AUDIO_RATE / 1000;
        int a = d.pred, b = c->n > 1 ? dec_next(&d) : a;
        uint32_t used = 2, ph = 0;
        while (!s_hush) {
            buf[k++] = (int16_t)(a + (b - a) * (int)ph / (int)one);
            ph += step;
            while (ph >= one) {
                ph -= one; a = b;
                if (used >= c->n) goto done;
                b = dec_next(&d); used++;
            }
            if (k == 256) { audio_play(buf, k); k = 0; }
        }
    }
done:
    if (k && !s_hush) audio_play(buf, k);
}

// map the bank and check it matches this firmware's group list; anything off = stay quiet
static void bank_attach(void)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
    const void *m;
    if (!p || p->size < fx_bank_off(p->size) + FX_BANK_MAX ||
        esp_partition_mmap(p, fx_bank_off(p->size), FX_BANK_MAX, ESP_PARTITION_MMAP_DATA, &m, &s_map) != ESP_OK) {
        ESP_LOGE(TAG, "can't map the sound bank"); return;
    }
    s_mapped = true;
    const bank_hdr_t *h = m;
    if (memcmp(h->magic, "SNB2", 4)) { ESP_LOGW(TAG, "no sound bank in flash yet (it downloads over WiFi)"); return; }
    if (h->layout != SND_LAYOUT || h->ngroups != SND_GROUPS) {
        ESP_LOGW(TAG, "sound bank is for a different build (layout %08x, want %08x) — waiting for the update",
                 (unsigned)h->layout, (unsigned)SND_LAYOUT);
        return;
    }
    s_grp = (const grp_t *)(h + 1);
    s_clip = (const clip_t *)(s_grp + h->ngroups);
    s_nclips = h->nclips;
    s_bank = m;
    ESP_LOGI(TAG, "sound bank: %u clips, voice %s", (unsigned)s_nclips, SND_VOICE);
}

static void bank_detach(void)
{
    s_bank = NULL;
    if (s_mapped) { esp_partition_munmap(s_map); s_mapped = false; }
}

bool fx_bank_ok(void) { return s_bank != NULL; }

// update.c is about to rewrite the bank: cut any sound, wait for the player to let go, unmap
void fx_bank_release(void)
{
    s_bank = NULL;
    fx_hush();
    for (int i = 0; i < 100 && s_talking; i++) vTaskDelay(pdMS_TO_TICKS(10));
    bank_detach();
}

void fx_bank_reload(void) { bank_detach(); bank_attach(); }

static void sound_task(void *arg)
{
    req_t r;
    for (;;) {
        xQueueReceive(s_sq, &r, portMAX_DELAY);
        if (r.g == SND_GROUPS) { s_hush = false; continue; }      // fx_hush() marker: the cut is done
        int64_t now = esp_timer_get_time();
        // narration that's fallen behind the arm is worse than silence
        if (now - r.at > (r.low ? 300000 : 2500000)) continue;
        if (r.g >= SND_GROUPS || !s_bank) continue;
        int n = s_grp[r.g].n, v;
        if (n <= 0) continue;
        if (r.pick >= 0) v = r.pick % n;
        else { v = rnd(0, n - 1); if (n > 1 && v == s_lastpick[r.g]) v = (v + 1) % n; }
        s_lastpick[r.g] = (uint8_t)v;
        uint32_t i = s_grp[r.g].first + v;
        if (i >= s_nclips) continue;
        s_talking = true;
        play_clip(i);
        s_talking = false;
    }
}

static void enqueue(int g, int pick, bool low)
{
    if (!s_sq || g < 0 || g >= SND_GROUPS) return;
    if (low && (s_talking || uxQueueMessagesWaiting(s_sq))) return;   // blips never wait in line
    req_t r = { (uint16_t)g, (int8_t)pick, low, esp_timer_get_time() };
    xQueueSend(s_sq, &r, 0);
}

static void play(int g) { enqueue(g, -1, false); }
void fx_play(int g) { play(g); }
bool fx_speaking(void) { return s_talking || (s_sq && uxQueueMessagesWaiting(s_sq)); }

static volatile int64_t s_quiet_until;
void fx_quiet(int minutes) { s_quiet_until = minutes > 0 ? esp_timer_get_time() + (int64_t)minutes * 60000000 : 0; }
bool fx_is_quiet(void) { return s_quiet_until && esp_timer_get_time() < s_quiet_until; }

void fx_say(int g)
{
    s_last_said_us = s_last_activity_us = esp_timer_get_time();
    // "be quiet" holds for everything but the lines about being quiet, and stopping
    if (fx_is_quiet() && g != SND_QUIET && g != SND_TALK_AGAIN && g != SND_STOPPED) return;
    enqueue(g, -1, false);
}

void fx_maybe(int g, int pct) { if (rnd(1, 100) <= pct) fx_say(g); }

static const char *COLOR_NAMES[SND_N_COLORS] = {"red", "yellow", "green", "blue", "purple"};

void fx_say_color(int g_color_first, int g_plain, const char *name)
{
    for (int c = 0; c < SND_N_COLORS; c++)
        if (name && strstr(name, COLOR_NAMES[c])) { fx_say(g_color_first + c); return; }
    fx_say(g_plain);
}

void fx_say_n(int g_first, int n) { if (n >= 1 && n <= SND_N_NUMBERS) fx_say(g_first + n - 1); }

// cut whatever is playing and drop what's queued; anything queued after this call still plays
void fx_hush(void)
{
    if (!s_sq) return;
    s_hush = true;
    xQueueReset(s_sq);
    req_t r = {SND_GROUPS, -1, 0, 0};
    xQueueSend(s_sq, &r, 0);
}

// ---------------------------------------------------------------- lights
typedef enum { L_BOOT, L_IDLE, L_THINK, L_WORK, L_HOLD, L_WARN, L_STOP, L_UPDATE, L_PARTY } led_mode_t;

static led_strip_handle_t s_led;
static volatile led_mode_t s_mode = L_BOOT;
static volatile int s_flash, s_show_t, s_show = -1;
static volatile bool s_working, s_listen;

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
            if (s_listen) { fill(SAPPHIRE, 0.25f + 0.55f * breathe); break; }   // ears open after "Hey Muffin"
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
        if (s_working && ++since_chirp > rnd(25, 45)) { enqueue(SND_S_CHIRP, -1, true); since_chirp = 0; }
        // idle chatter: once, a few minutes after people stop playing (not when nobody's been around)
        if (CONFIG_XARM_CHATTER_MIN > 0 && s_mode == L_IDLE && t % 30 == 0) {
            int64_t now = esp_timer_get_time(), quiet = now - s_last_activity_us;
            static int64_t s_chatted_after;
            if (s_last_activity_us && s_chatted_after != s_last_activity_us &&
                quiet > (int64_t)CONFIG_XARM_CHATTER_MIN * 60000000 && quiet < 20LL * 60000000) {
                s_chatted_after = s_last_activity_us;
                if (!fx_is_quiet()) { chat_idle(); voice_engage(); }   // a line, or a question it'll act on if you answer
            }
        }
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
    bank_attach();
    memset(s_lastpick, 0xFF, sizeof s_lastpick);
    s_sq = xQueueCreate(8, sizeof(req_t));
    xTaskCreate(sound_task, "fx_snd", 4096, NULL, 4, NULL);
}

static bool is(const char *a, const char *b) { return !strcmp(a, b); }

void fx_state(const char *st)
{
    led_mode_t was = s_mode;
    s_working = false;
    if (!is(st, "idle")) s_last_activity_us = esp_timer_get_time();
    if      (is(st, "searching"))                                   { s_mode = L_THINK;  s_working = true; }
    else if (is(st, "moving") || is(st, "dropping") || is(st, "sorting") || is(st, "giving") || is(st, "stacking"))
                                                                    { s_mode = L_WORK;   s_working = true; }
    else if (is(st, "grasping"))                                    { s_mode = L_WORK;   play(SND_S_GRAB); s_working = true; }
    else if (is(st, "holding"))                                     { s_mode = L_HOLD;   play(SND_S_HAPPY); }
    else if (is(st, "waving"))                                      { s_mode = L_PARTY;  play(SND_S_WAVE); }
    else if (is(st, "bowing"))                                      { s_mode = L_PARTY;  play(SND_S_BOW); }
    else if (is(st, "dancing"))                                     { s_mode = L_PARTY;  play(SND_S_DANCE); }
    else if (is(st, "helping"))                                     { s_mode = L_HOLD; }
    else if (is(st, "teaching"))                                    { s_mode = L_THINK; }
    else if (is(st, "ringing") || is(st, "playing"))                { s_mode = L_PARTY;  s_working = is(st, "playing"); }
    else if (is(st, "not_found") || is(st, "need_calib"))           { s_mode = L_WARN;   play(SND_S_SAD); }
    else if (is(st, "stopped"))                                     { s_mode = L_STOP;   fx_hush(); play(SND_S_STOP); fx_say(SND_STOPPED); }
    else if (is(st, "updating"))                                    { s_mode = L_UPDATE; play(SND_S_THINK); }
    else if (is(st, "idle"))                                        { if (was == L_WORK) play(SND_S_DONE); s_mode = L_IDLE; }
}

void fx_event(fx_event_t e)
{
    switch (e) {
    case FX_BOOT:  s_mode = L_BOOT; play(SND_S_BOOT); fx_say(SND_BOOT); break;
    case FX_HEARD: s_flash = 10; s_last_activity_us = esp_timer_get_time(); enqueue(SND_S_HEARD, -1, true); break;
    case FX_ERROR: s_mode = L_WARN; play(SND_S_SAD); fx_say(SND_ERROR); break;
    case FX_BELL:  play(SND_S_STRIKE); break;
    case FX_CHEER: s_mode = L_PARTY; play(SND_S_CHEER); fx_say(SND_CHEER); break;
    case FX_UPDATED: s_mode = L_PARTY; play(SND_S_FANFARE); fx_say(SND_UPDATED); break;
    }
}

void fx_show(int j)
{
    s_show = j & 3;
    s_show_t = 14;            // ~0.45 s
    enqueue(SND_S_SHOW, j & 3, false);
}

void fx_greet(int jewel, unsigned seed)
{
    s_show = jewel & 3;
    s_show_t = 60;            // ~2 s in their color
    enqueue(SND_S_GREET, (int)(seed % 64), false);   // their own jingle, always the same one
    fx_say(SND_GREET);
}

void fx_listening(bool on) { s_listen = on; }
