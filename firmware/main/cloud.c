// cloud.c — Whisper speech-to-text over HTTPS (see cloud.h). One request at a time, on its own task, so the
// voice task never waits on the network and "stop" stays instant.
#include "cloud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "store.h"
#include "router.h"
#include "school.h"
#include "fx.h"
#include "sdkconfig.h"

void voice_cloud_heard(const char *text, int err, const cloud_clip_t *clip);   // voice.c

#define MAX_SAMPLES (CLOUD_RATE * CLOUD_MAX_MS / 1000)
#define RESP_MAX    16384
#define BOUNDARY    "----muffin7f3a9c"
#define DAY_US      (24LL * 3600 * 1000000)

static const char *TAG = "cloud";
static char s_key[160];
static bool s_on = true, s_key_bad;
static volatile bool s_busy;
static int64_t s_backoff_until, s_day_start;
static int s_today, s_ms;
static volatile int64_t s_started;
static const char *volatile s_phase = "";
static char s_status[72] = "", s_last[96] = "";
static char s_hint[160];                      // what Muffin is waiting to hear (School): biases Whisper and the intent check
static cloud_clip_t s_job;
static int16_t *s_pcm;
static char *s_resp;
static TaskHandle_t s_task;
static esp_http_client_handle_t s_http;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void status(const char *s) { portENTER_CRITICAL(&s_mux); strlcpy(s_status, s, sizeof s_status); portEXIT_CRITICAL(&s_mux); }

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open("xarm", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof s_key;
    if (nvs_get_str(h, "stt_key", s_key, &n) != ESP_OK) s_key[0] = 0;
    uint8_t v;
    if (nvs_get_u8(h, "cloud", &v) == ESP_OK) s_on = v;
    nvs_close(h);
}

void cloud_set_key(const char *key)
{
    // keep only the printable part (pasting from a phone can bring spaces / newlines along)
    char k[sizeof s_key];
    size_t j = 0;
    for (const char *p = key; *p && j + 1 < sizeof k; p++) if (*p > ' ' && *p < 127) k[j++] = *p;
    k[j] = 0;
    strlcpy(s_key, k, sizeof s_key);
    s_key_bad = false;
    s_backoff_until = 0;
    status(k[0] ? "key saved — say “Hey Muffin”" : "");
    nvs_handle_t h;
    if (nvs_open("xarm", NVS_READWRITE, &h) == ESP_OK) {
        if (k[0]) nvs_set_str(h, "stt_key", k); else nvs_erase_key(h, "stt_key");
        nvs_commit(h);
        nvs_close(h);
    }
    if (s_http) { esp_http_client_cleanup(s_http); s_http = NULL; }   // new key = fresh connection
}

void cloud_enable(bool on)
{
    s_on = on;
    nvs_handle_t h;
    if (nvs_open("xarm", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "cloud", on); nvs_commit(h); nvs_close(h); }
}

static void day_roll(int64_t now)
{
    if (now - s_day_start > DAY_US) { s_day_start = now; s_today = 0; }
}

bool cloud_ready(void)
{
    if (!s_on || !s_key[0] || s_key_bad || !s_task || !net_online()) return false;
    int64_t now = esp_timer_get_time();
    day_roll(now);
    return now >= s_backoff_until && s_today < CONFIG_XARM_STT_DAILY_MAX;
}

bool cloud_busy(void) { return s_busy; }

bool cloud_submit(const cloud_clip_t *c)
{
    if (s_busy || !s_task || !c->n) return false;
    s_busy = true;
    s_job = *c;
    s_job.n = c->n < MAX_SAMPLES ? c->n : MAX_SAMPLES;
    memcpy(s_pcm, c->pcm, s_job.n * sizeof(int16_t));
    s_job.pcm = s_pcm;
    xTaskNotifyGive(s_task);
    return true;
}

void cloud_diag(bool *on, char *key_hint, size_t nk, char *st, size_t ns, char *last, size_t nl, int *ms, int *today)
{
    *on = s_on;
    size_t kl = strlen(s_key);
    if (kl > 8) snprintf(key_hint, nk, "%.4s…%s", s_key, s_key + kl - 4);   // "gsk_…wxyz" — never the whole key
    else key_hint[0] = 0;
    int64_t now = esp_timer_get_time();
    if (!s_key[0])               strlcpy(st, "no key — paste your Groq key below", ns);
    else if (!s_on)              strlcpy(st, "off — using the on-board ears", ns);
    else if (s_key_bad)          strlcpy(st, "Groq turned the key down — check it and paste it again", ns);
    else if (!net_online())      strlcpy(st, "no internet — using the on-board ears", ns);
    else if (s_today >= CONFIG_XARM_STT_DAILY_MAX) strlcpy(st, "daily limit reached — on-board ears until tomorrow", ns);
    else if (s_busy && now - s_started > 3000000)
        snprintf(st, ns, "%s… %d s", s_phase[0] ? s_phase : "working", (int)((now - s_started) / 1000000));
    else if (now < s_backoff_until) snprintf(st, ns, "%s — on-board ears for %d s", s_status, (int)((s_backoff_until - now) / 1000000));
    else { portENTER_CRITICAL(&s_mux); strlcpy(st, s_status[0] ? s_status : "ready", ns); portEXIT_CRITICAL(&s_mux); }
    portENTER_CRITICAL(&s_mux);
    strlcpy(last, s_last, nl);
    portEXIT_CRITICAL(&s_mux);
    *ms = s_ms;
    *today = s_today;
}

// ---------------------------------------------------------------- the request
static void wav_header(uint8_t h[44], int samples)
{
    uint32_t data = samples * 2, riff = 36 + data, rate = CLOUD_RATE, byterate = CLOUD_RATE * 2, fmtlen = 16;
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 16, &fmtlen, 4); memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byterate, 4); memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
}

static bool write_all(esp_http_client_handle_t h, const char *p, int n)
{
    while (n > 0) {
        int w = esp_http_client_write(h, p, n > 4096 ? 4096 : n);
        if (w <= 0) return false;
        p += w; n -= w;
    }
    return true;
}

void cloud_hint(const char *words)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_hint, words ? words : "", sizeof s_hint);
    portEXIT_CRITICAL(&s_mux);
}

// Whisper's word list hint: the things kids actually say to Muffin, so "muffin" isn't "muffet" and
// "dance robot" isn't "dance, Rob". Slot names (the camera colors / objects) are added at request time.
static void build_prompt(char *p, size_t n)
{
    char hint[sizeof s_hint];
    portENTER_CRITICAL(&s_mux);
    strlcpy(hint, s_hint, sizeof hint);
    portEXIT_CRITICAL(&s_mux);
    int k = snprintf(p, n, "%s%sHey Muffin. Pick up the %s. Grab the %s. Dance robot. Wave hello. Take a bow. "
                     "Ring the bell. Go home. Stop. Build a tower. Sort everything. Hold this for me. "
                     "Tell me a joke. Teach me about robots. Quiz me. That's true. That's false. Yes please. No thanks.",
                     hint, hint[0] ? " " : "",       // what it's waiting for goes first: Whisper leans hardest on the start
                     g_cfg.slot_name[0][0] ? g_cfg.slot_name[0] : "red", g_cfg.slot_name[1][0] ? g_cfg.slot_name[1] : "blue");
    (void)k;
}

// one attempt. returns the HTTP status, or -1 if the connection failed before an answer came back
static int attempt(const int16_t *pcm, int n, int *got)
{
    if (!s_http) {
        char url[] = CONFIG_XARM_STT_URL;
        esp_http_client_config_t c = {
            .url = url, .method = HTTP_METHOD_POST, .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 10000, .keep_alive_enable = true, .buffer_size = 2048, .buffer_size_tx = 1536,
        };
        s_http = esp_http_client_init(&c);
        if (!s_http) return -1;
    }
    esp_http_client_handle_t h = s_http;
    char auth[sizeof s_key + 8];
    snprintf(auth, sizeof auth, "Bearer %s", s_key);
    esp_http_client_set_method(h, HTTP_METHOD_POST);
    esp_http_client_set_header(h, "Authorization", auth);
    esp_http_client_set_header(h, "Content-Type", "multipart/form-data; boundary=" BOUNDARY);

    char prompt[600];
    build_prompt(prompt, sizeof prompt);
    char *pre = malloc(1400);
    if (!pre) return -1;
    int npre = snprintf(pre, 1400,
        "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n"
        "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nen\r\n"
        "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"temperature\"\r\n\r\n0\r\n"
        "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\nverbose_json\r\n"
        "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"prompt\"\r\n\r\n%s\r\n"
        "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"muffin.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", CONFIG_XARM_STT_MODEL, prompt);
    static const char post[] = "\r\n--" BOUNDARY "--\r\n";
    uint8_t wav[44];
    wav_header(wav, n);
    int total = npre + 44 + n * 2 + (int)sizeof post - 1;

    int code = -1;
    s_phase = "connecting to Groq";
    if (esp_http_client_open(h, total) == ESP_OK && (s_phase = "sending", true)
        && write_all(h, pre, npre) && write_all(h, (const char *)wav, 44)
        && write_all(h, (const char *)pcm, n * 2) && write_all(h, post, sizeof post - 1)
        && (s_phase = "waiting for Groq", true) && esp_http_client_fetch_headers(h) >= 0) {
        s_phase = "reading answer";
        code = esp_http_client_get_status_code(h);
        int r, k = 0;
        while (k < RESP_MAX - 1 && (r = esp_http_client_read(h, s_resp + k, RESP_MAX - 1 - k)) > 0) k += r;
        s_resp[k] = 0;
        *got = k;
        if (!esp_http_client_is_complete_data_received(h)) esp_http_client_close(h);   // don't reuse a half-read stream
    }
    free(pre);
    if (code < 0) { esp_http_client_cleanup(h); s_http = NULL; }
    return code;
}

// Whisper's own "this was noise" verdict: every segment silent-looking -> nothing was said. *unsure: real
// speech, but Whisper wasn't confident in its words (low average log-probability): check them before acting.
static void text_from(const char *json, char *out, size_t n, bool *unsure)
{
    out[0] = 0;
    *unsure = false;
    cJSON *j = cJSON_Parse(json);
    if (!j) return;
    cJSON *t = cJSON_GetObjectItem(j, "text"), *segs = cJSON_GetObjectItem(j, "segments");
    bool any_real = !cJSON_IsArray(segs) || cJSON_GetArraySize(segs) == 0;
    cJSON *s;
    cJSON_ArrayForEach(s, segs) {
        cJSON *ns = cJSON_GetObjectItem(s, "no_speech_prob"), *lp = cJSON_GetObjectItem(s, "avg_logprob");
        bool silent = cJSON_IsNumber(ns) && ns->valuedouble > 0.6 && cJSON_IsNumber(lp) && lp->valuedouble < -1.0;
        if (!silent) any_real = true;
        if (!silent && cJSON_IsNumber(lp) && lp->valuedouble < -0.9) *unsure = true;
    }
    if (cJSON_IsString(t) && any_real) strlcpy(out, t->valuestring, n);
    cJSON_Delete(j);
}

static void task(void *arg)
{
    char text[160];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int64_t t0 = esp_timer_get_time();
        s_started = t0;
        day_roll(t0);
        s_today++;
        int got = 0, code = attempt(s_job.pcm, s_job.n, &got);
        if (code < 0) code = attempt(s_job.pcm, s_job.n, &got);     // a stale keep-alive socket: once more, fresh
        int64_t t1 = esp_timer_get_time();
        int err = 0;
        bool unsure = false;
        text[0] = 0;
        if (code == 200) {
            text_from(s_resp, text, sizeof text, &unsure);
            s_ms = (int)((t1 - t0) / 1000);
            portENTER_CRITICAL(&s_mux);
            strlcpy(s_last, text, sizeof s_last);
            portEXIT_CRITICAL(&s_mux);
            status("ready");
            ESP_LOGI(TAG, "\"%s\" (%.1f s audio, %d ms)", text, s_job.n / (float)CLOUD_RATE, s_ms);
        } else {
            err = code;
            if (code == 401 || code == 403) { s_key_bad = true; status("key rejected"); }
            else if (code == 429) { s_backoff_until = t1 + 60 * 1000000LL; status("Groq's free limit hit"); }
            else if (code < 0) { s_backoff_until = t1 + 20 * 1000000LL; status("can't reach Groq"); }
            else { s_backoff_until = t1 + 20 * 1000000LL; char b[48]; snprintf(b, sizeof b, "Groq error %d", code); status(b); }
            ESP_LOGW(TAG, "request failed: %d %.200s", code, got ? s_resp : "");
        }
        cloud_clip_t done = s_job;    // the next clip may land in s_job as soon as busy clears
        done.unsure = unsure;
        s_phase = "";
        s_busy = false;
        voice_cloud_heard(text, err, &done);
    }
}

// ---------------------------------------------------------------- intent: any wording -> one of Muffin's commands
// A fast language model (Groq, same key) picks ONE line from a closed list: a command Muffin knows, LESSON <n>
// for a question about robots / math / physics, OFFTOPIC for any other question, or NONE. It never writes an
// answer: Muffin's voice is pre-recorded, and anything that isn't exactly one of those lines is thrown away.
static esp_http_client_handle_t s_llm;
static char s_phr[150][48];

static int intent_attempt(const char *body, int *got)
{
    if (!s_llm) {
        esp_http_client_config_t c = {
            .url = CONFIG_XARM_INTENT_URL, .method = HTTP_METHOD_POST, .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 6000, .keep_alive_enable = true, .buffer_size = 2048, .buffer_size_tx = 2048,
        };
        s_llm = esp_http_client_init(&c);
        if (!s_llm) return -1;
    }
    char auth[sizeof s_key + 8];
    snprintf(auth, sizeof auth, "Bearer %s", s_key);
    esp_http_client_set_method(s_llm, HTTP_METHOD_POST);
    esp_http_client_set_header(s_llm, "Authorization", auth);
    esp_http_client_set_header(s_llm, "Content-Type", "application/json");
    int len = (int)strlen(body), code = -1;
    if (esp_http_client_open(s_llm, len) == ESP_OK && write_all(s_llm, body, len) && esp_http_client_fetch_headers(s_llm) >= 0) {
        code = esp_http_client_get_status_code(s_llm);
        int r, k = 0;
        while (k < RESP_MAX - 1 && (r = esp_http_client_read(s_llm, s_resp + k, RESP_MAX - 1 - k)) > 0) k += r;
        s_resp[k] = 0;
        *got = k;
        if (!esp_http_client_is_complete_data_received(s_llm)) esp_http_client_close(s_llm);
    }
    if (code < 0) { esp_http_client_cleanup(s_llm); s_llm = NULL; }
    return code;
}

int cloud_intent(const char *heard, char *out, size_t n)
{
    out[0] = 0;
    if (!cloud_ready() || !s_resp || !heard[0]) return INTENT_NONE;
    // Muffin's commands, minus the grown-up passphrase (never sent anywhere, never pickable) and "hi i'm <kid>"
    int np = 0;
    {
        int all = router_phrases(s_phr, 150);
        const char *pass = school_pass();
        for (int i = 0; i < all; i++) {
            if ((pass[0] && !strcmp(s_phr[i], pass)) || !strncmp(s_phr[i], "hi i'm", 6)) continue;
            if (np != i) strlcpy(s_phr[np], s_phr[i], sizeof s_phr[np]);
            np++;
        }
    }
    char hint[sizeof s_hint];
    portENTER_CRITICAL(&s_mux);
    strlcpy(hint, s_hint, sizeof hint);
    portEXIT_CRITICAL(&s_mux);
    // the instructions, the closed lists, and what it heard
    size_t cap = 9000;
    char *sys = malloc(cap);
    if (!sys) return INTENT_NONE;
    int k = snprintf(sys, cap,
        "You are the ears of Muffin, a robot arm that teaches a child robotics, math and physics. The child just said "
        "something (a speech-to-text transcript, maybe misheard). Reply with exactly ONE line and nothing else:\n"
        "- one command copied exactly from COMMANDS, if they asked Muffin to do something or answered its question;\n"
        "- LESSON <number> if they asked a question about robots, machines, electronics, math or physics "
        "(the closest lesson from LESSONS);\n"
        "- OFFTOPIC if they asked a question about anything else;\n"
        "- NONE if it is unclear.\n"
        "Never answer a question yourself. Never write anything that is not one of those.\n");
    if (hint[0]) k += snprintf(sys + k, cap - k, "Muffin is waiting for one of these answers: %s\n", hint);
    k += snprintf(sys + k, cap - k, "COMMANDS:");
    for (int i = 0; i < np && k < (int)cap - 64; i++) k += snprintf(sys + k, cap - k, "%s %s", i ? "," : "", s_phr[i]);
    k += snprintf(sys + k, cap - k, "\nLESSONS:");
    for (int i = 0; i < SCHOOL_COUNT && k < (int)cap - 80; i++) k += snprintf(sys + k, cap - k, " %d %s;", i + 1, SCHOOL_TOPIC[i]);
    cJSON *b = cJSON_CreateObject(), *msgs = cJSON_AddArrayToObject(b, "messages"), *m;
    cJSON_AddStringToObject(b, "model", CONFIG_XARM_INTENT_MODEL);
    cJSON_AddNumberToObject(b, "temperature", 0);
    cJSON_AddNumberToObject(b, "max_tokens", 16);
    m = cJSON_CreateObject(); cJSON_AddStringToObject(m, "role", "system"); cJSON_AddStringToObject(m, "content", sys); cJSON_AddItemToArray(msgs, m);
    m = cJSON_CreateObject(); cJSON_AddStringToObject(m, "role", "user"); cJSON_AddStringToObject(m, "content", heard); cJSON_AddItemToArray(msgs, m);
    free(sys);
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    if (!body) return INTENT_NONE;
    int got = 0, code = intent_attempt(body, &got);
    if (code < 0) code = intent_attempt(body, &got);
    cJSON_free(body);
    if (code != 200) { ESP_LOGW(TAG, "intent: %d", code); return INTENT_NONE; }
    char ans[64] = "";
    cJSON *j = cJSON_Parse(s_resp), *c = j ? cJSON_GetObjectItem(j, "choices") : NULL;
    cJSON *msg = cJSON_IsArray(c) && cJSON_GetArraySize(c) ? cJSON_GetObjectItem(cJSON_GetArrayItem(c, 0), "message") : NULL;
    cJSON *ct = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
    if (cJSON_IsString(ct)) {
        // lower-case, letters/digits/spaces/apostrophes only, first line
        size_t w = 0;
        for (const char *p = ct->valuestring; *p && *p != '\n' && w < sizeof ans - 1; p++) {
            char ch = *p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p;
            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == '\'') ans[w++] = ch;
        }
        while (w && ans[w - 1] == ' ') w--;
        ans[w] = 0;
    }
    cJSON_Delete(j);
    const char *a = ans;
    while (*a == ' ') a++;
    ESP_LOGI(TAG, "intent: \"%s\" -> \"%s\"", heard, a);
    if (!strcmp(a, "offtopic")) return INTENT_OFFTOPIC;
    if (!strncmp(a, "lesson ", 7)) {
        int l = atoi(a + 7);
        if (l >= 1 && l <= SCHOOL_COUNT) { snprintf(out, n, "%d", l - 1); return INTENT_LESSON; }
        return INTENT_NONE;
    }
    for (int i = 0; i < np; i++) if (!strcmp(a, s_phr[i])) { strlcpy(out, s_phr[i], n); return INTENT_COMMAND; }   // only its own list
    return INTENT_NONE;
}

void cloud_init(void)
{
    nvs_load();
    s_pcm = heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_resp = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM);
    if (!s_pcm || !s_resp) { ESP_LOGE(TAG, "no PSRAM for cloud buffers"); return; }
    // TLS handshakes want a deep stack in internal RAM. Not pinned: core 0 is busy with the AFE (echo
    // cancelling, VADNet) and WiFi, and a request pinned there starved — the same as the update task, which floats.
    if (xTaskCreate(task, "cloud", 12288, NULL, 5, &s_task) != pdPASS) s_task = NULL;
    ESP_LOGI(TAG, "cloud ears: %s, %s", s_key[0] ? "key set" : "no key", s_on ? "on" : "off");
}
