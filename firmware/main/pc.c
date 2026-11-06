// pc.c — Johnny: the shop PC is the brain, this board is the arm's hands, ears and mouth. Protocol in pc.h.
#include "pc.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "audio.h"
#include "router.h"
#include "pick.h"
#include "moves.h"
#include "cam.h"
#include "store.h"
#include "fx.h"
#include "radar.h"
#include "voice.h"
#include "eye.h"

static const char *TAG = "pc";

#define SPK_BYTES (256 * 1024)      // 8 s of speech queued; the PC paces itself at ~1.5x real time
#define MIC_BYTES (32 * 1024)       // 1 s of mic in flight to the PC

static httpd_handle_t s_hd;
static volatile int s_fd = -1;
static SemaphoreHandle_t s_tx;      // one writer on the socket at a time (httpd task + mic sender)
static StreamBufferHandle_t s_spk, s_mic;
static volatile bool s_hush, s_playing, s_spoke, s_stopped;
static volatile float s_lvl;
static volatile int64_t s_since_us;
static bool s_here = true, s_resting, s_put_down;   // the radar: someone at the bench / camera resting / held part set down

bool pc_connected(void) { return s_fd >= 0; }
bool pc_speaking(void) { return s_playing || (s_spk && xStreamBufferBytesAvailable(s_spk) > 0); }
float pc_talk_level(void) { return s_playing ? s_lvl : 0; }

static void drop(const char *why)
{
    if (s_fd < 0) return;
    ESP_LOGW(TAG, "PC gone (%s)", why);
    s_fd = -1;
    s_hush = true;
    fx_pc(FX_PC_NONE);
}

void pc_sock_closed(int fd) { if (fd == s_fd) drop("socket closed"); }

static bool send_raw(httpd_ws_type_t type, const void *p, size_t n)
{
    int fd = s_fd;
    if (fd < 0) return false;
    httpd_ws_frame_t f = { .final = true, .type = type, .payload = (uint8_t *)p, .len = n };
    xSemaphoreTake(s_tx, portMAX_DELAY);
    esp_err_t e = fd == s_fd ? httpd_ws_send_frame_async(s_hd, fd, &f) : ESP_FAIL;
    xSemaphoreGive(s_tx);
    if (e != ESP_OK && fd == s_fd) { drop("send failed"); httpd_sess_trigger_close(s_hd, fd); }
    return e == ESP_OK;
}

static void send_json(cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    if (s) { send_raw(HTTPD_WS_TYPE_TEXT, s, strlen(s)); free(s); }
    cJSON_Delete(j);
}

static void ev(const char *name) { cJSON *j = cJSON_CreateObject(); cJSON_AddStringToObject(j, "ev", name); send_json(j); }

static cJSON *state_obj(void)
{
    char st[24], d[96];
    pick_status(st, sizeof st, d, sizeof d);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "ev", "state");
    cJSON_AddStringToObject(j, "state", st);
    cJSON_AddStringToObject(j, "detail", d);
    cJSON_AddBoolToObject(j, "holding", pick_holding());
    cJSON_AddBoolToObject(j, "busy", !pick_idle());
    return j;
}

static void hello(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "ev", "hello");
    cJSON_AddStringToObject(j, "name", CONFIG_XARM_NAME);
    cJSON_AddStringToObject(j, "version", esp_app_get_description()->version);
    cJSON_AddNumberToObject(j, "rate", AUDIO_RATE);
    cJSON_AddBoolToObject(j, "camera", cam_enabled());
    cJSON_AddBoolToObject(j, "radar", radar_known());        // false: no radar we understand, always listen
    cJSON_AddBoolToObject(j, "present", s_here);
    cJSON *sl = cJSON_AddArrayToObject(j, "slots");    // colors it can pick: "pick up the <slot>"
    for (int i = 0; i < NSLOT; i++) if (g_cfg.slot_name[i][0]) cJSON_AddItemToArray(sl, cJSON_CreateString(g_cfg.slot_name[i]));
    cJSON *m = cJSON_AddArrayToObject(j, "moves");      // recorded tricks: "do the <name>"
    for (int i = 0; i < MOVE_MAX; i++) if (moves_name(i)[0]) cJSON_AddItemToArray(m, cJSON_CreateString(moves_name(i)));
    send_json(j);
    send_json(state_obj());
}

// ---------------------------------------------------------------- ears: AFE output -> PC
void pc_mic(const int16_t *pcm, int n)
{
    if (s_fd < 0 || !s_mic) return;
    xStreamBufferSend(s_mic, pcm, n * sizeof(int16_t), 0);   // full = the PC fell behind: drop, never block the ears
}

// ---------------------------------------------------------------- the radar: who's at the bench
#define REST_AFTER_S 60             // nobody for a minute: camera stops, ring goes nearly dark
#define PUTDOWN_AFTER_S 180         // holding a part and nobody for 3 minutes: set it down at the drop spot
#define FOLLOW_MIN_CM 50            // closer than this, stop turning toward the person
#define REACH_NEAR_CM 45            // closer than this, don't reach toward them at all

static void radar_event(bool here)
{
    if (s_fd < 0) return;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "ev", "radar");
    cJSON_AddBoolToObject(j, "present", here);
    cJSON_AddNumberToObject(j, "cm", radar_distance_cm());
    send_json(j);
}

static void presence_tick(void)
{
    int away = radar_away_s();                 // -1: no radar reading we understand -> act as if someone's here
    bool here = away <= 0;
    if (here != s_here) {
        s_here = here;
        radar_event(here);
        if (here) pick_attention();            // someone walked up: perk up (wakes him from a nap)
    }
    // follow: the nearest person's angle (LD2450) turns the resting pose's base, like "turn toward voices" does.
    // Workshop -> Voice -> Turn toward voices sets the direction (1, flipped -1) or switches it off (0).
    // Closer than FOLLOW_MIN_CM: hold the heading (no turning toward a hand that's right at the arm).
    int deg, cm = radar_distance_cm(), dir = voice_face();
    if (here && dir && radar_angle(&deg) && cm >= FOLLOW_MIN_CM) pick_face(dir * deg / 35.0f);
    // reach out toward them: never at someone right at the arm, most for someone a step or two away
    float reach = !here || radar_away_s() != 0 ? 0
                : cm < 0 ? 0.3f : cm < REACH_NEAR_CM ? 0 : cm < 80 ? 0.3f : cm < 200 ? 0.8f : 0.4f;
    pick_reach(reach);
    bool rest = away >= REST_AFTER_S;
    if (rest != s_resting) { s_resting = rest; eye_pause(rest); fx_away(rest); }
    if (here) s_put_down = false;
    else if (!s_put_down && away >= PUTDOWN_AFTER_S && pick_holding() && pick_idle()) {
        char msg[96] = "";
        s_put_down = true;
        router_exec_ex("put it down", msg, sizeof msg, true);
        ESP_LOGI(TAG, "nobody here for %d s and I'm holding something: put it down -> %s", away, msg);
    }
}

static void tx_task(void *arg)
{
    static uint8_t buf[2048];                  // 64 ms per frame
    char last[24] = "";
    int64_t last_state = 0;
    for (;;) {
        size_t n = xStreamBufferReceive(s_mic, buf, sizeof buf, pdMS_TO_TICKS(60));
        static int64_t last_presence;
        if (esp_timer_get_time() - last_presence > 500000) { last_presence = esp_timer_get_time(); presence_tick(); }
        voice_pump();                          // "stop" and other commands the ears heard (they can't run on its PSRAM stack)
        if (s_fd < 0) { last[0] = 0; continue; }
        if (n) send_raw(HTTPD_WS_TYPE_BINARY, buf, n & ~1u);
        if (s_spoke) { s_spoke = false; ev("spoke"); }
        if (s_stopped) { s_stopped = false; ev("stop"); }
        int64_t now = esp_timer_get_time();
        if (now - last_state > 250000) {        // the arm's job changed: tell the brain
            last_state = now;
            char st[24], d[8];
            pick_status(st, sizeof st, d, sizeof d);
            if (strcmp(st, last)) { strlcpy(last, st, sizeof last); send_json(state_obj()); }
        }
    }
}

// ---------------------------------------------------------------- mouth: PC speech -> speaker
static void spk_task(void *arg)
{
    static int16_t pcm[256];
    int64_t quiet_since = 0;
    for (;;) {
        if (s_hush) { s_hush = false; xStreamBufferReset(s_spk); }
        size_t n = xStreamBufferReceive(s_spk, pcm, sizeof pcm, pdMS_TO_TICKS(20));
        int k = (int)(n / 2);
        if (k > 0) {
            if (!s_playing) { s_playing = true; fx_pc(FX_PC_TALK); }
            float acc = 0;
            for (int i = 0; i < k; i++) acc += (float)pcm[i] * pcm[i];
            float r = sqrtf(acc / k) / 6000.0f;
            s_lvl = r > 1 ? 1 : r;
            audio_play(pcm, k);
            quiet_since = esp_timer_get_time();
        } else if (s_playing && esp_timer_get_time() - quiet_since > 200000) {   // a gap = the reply is done
            s_playing = false; s_lvl = 0; s_spoke = true;
            fx_pc(s_fd >= 0 ? FX_PC_IDLE : FX_PC_NONE);
        }
    }
}

void pc_stopped(void)
{
    s_hush = true;
    if (s_fd >= 0) s_stopped = true;
}

// ---------------------------------------------------------------- the socket
static void on_text(httpd_req_t *r, const char *txt)
{
    cJSON *j = cJSON_Parse(txt), *v;
    if (!j) return;
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "cmd"))) {
        char msg[96] = "";
        bool ok = router_exec_ex(v->valuestring, msg, sizeof msg, true);   // the PC is the grown-up's controller
        ESP_LOGI(TAG, "cmd \"%s\" -> %s", v->valuestring, msg);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ev", "cmd");
        cJSON_AddStringToObject(o, "text", v->valuestring);
        cJSON_AddBoolToObject(o, "ok", ok);
        cJSON_AddStringToObject(o, "msg", msg);
        cJSON *id = cJSON_GetObjectItem(j, "id");
        if (cJSON_IsNumber(id)) cJSON_AddNumberToObject(o, "id", id->valuedouble);
        send_json(o);
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(j, "hush"))) s_hush = true;
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "led"))) {
        const char *m = v->valuestring;
        if (!s_playing) fx_pc(!strcmp(m, "listen") ? FX_PC_LISTEN : !strcmp(m, "think") ? FX_PC_THINK : FX_PC_IDLE);
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(j, "attn"))) pick_attention();
    if (cJSON_IsBool(v = cJSON_GetObjectItem(j, "listening"))) pick_listening(cJSON_IsTrue(v));
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "volume"))) audio_volume(v->valueint);
    if (cJSON_IsTrue(cJSON_GetObjectItem(j, "state"))) send_json(state_obj());
    cJSON_Delete(j);
}

static esp_err_t h_ws(httpd_req_t *r)
{
    if (r->method == HTTP_GET) {               // handshake done: this PC is the brain now
        int fd = httpd_req_to_sockfd(r), old = s_fd;
        s_fd = fd;
        s_since_us = esp_timer_get_time();
        if (old >= 0 && old != fd) httpd_sess_trigger_close(s_hd, old);
        ESP_LOGI(TAG, "PC connected (socket %d)", fd);
        fx_pc(FX_PC_IDLE);
        hello();
        return ESP_OK;
    }
    httpd_ws_frame_t f = {0};
    if (httpd_ws_recv_frame(r, &f, 0) != ESP_OK) return ESP_FAIL;
    if (f.type == HTTPD_WS_TYPE_CLOSE) { pc_sock_closed(httpd_req_to_sockfd(r)); return ESP_OK; }
    if (!f.len) return ESP_OK;
    if (f.len > 64 * 1024) return ESP_FAIL;
    uint8_t *b = heap_caps_malloc(f.len + 1, MALLOC_CAP_SPIRAM);
    if (!b) return ESP_ERR_NO_MEM;
    f.payload = b;
    esp_err_t e = httpd_ws_recv_frame(r, &f, f.len);
    if (e == ESP_OK) {
        if (httpd_req_to_sockfd(r) != s_fd) { /* an old brain still talking: ignore */ }
        else if (f.type == HTTPD_WS_TYPE_BINARY) xStreamBufferSend(s_spk, b, f.len & ~1u, pdMS_TO_TICKS(1000));
        else if (f.type == HTTPD_WS_TYPE_TEXT) { b[f.len] = 0; on_text(r, (const char *)b); }
    }
    free(b);
    return e;
}

void pc_register(httpd_handle_t h)
{
    s_hd = h;
    s_tx = xSemaphoreCreateMutex();
    s_spk = xStreamBufferCreateWithCaps(SPK_BYTES, 2, MALLOC_CAP_SPIRAM);
    s_mic = xStreamBufferCreateWithCaps(MIC_BYTES, 2048, MALLOC_CAP_SPIRAM);
    if (!s_tx || !s_spk || !s_mic) { ESP_LOGE(TAG, "no memory for the PC link"); return; }
    xTaskCreate(spk_task, "pc_spk", 4096, NULL, 5, NULL);
    xTaskCreate(tx_task, "pc_tx", 4096, NULL, 3, NULL);
    httpd_uri_t u = { .uri = "/ws", .method = HTTP_GET, .handler = h_ws, .is_websocket = true };
    httpd_register_uri_handler(h, &u);
    fx_pc(FX_PC_NONE);
    ESP_LOGI(TAG, "waiting for the PC brain on ws://%s.local/ws", CONFIG_XARM_NAME);
}

void pc_json(cJSON *o)
{
    cJSON *b = cJSON_AddObjectToObject(o, "brain");
    cJSON_AddStringToObject(b, "name", CONFIG_XARM_NAME);
    cJSON_AddBoolToObject(b, "pc", s_fd >= 0);
    cJSON_AddNumberToObject(b, "since_s", s_fd >= 0 ? (double)((esp_timer_get_time() - s_since_us) / 1000000) : 0);
    cJSON_AddBoolToObject(b, "talking", s_playing);
}
