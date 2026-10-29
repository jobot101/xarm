#include "update.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "pick.h"
#include "fx.h"
#include "sdkconfig.h"

static const char *TAG = "update";
static TaskHandle_t s_task;
static bool s_auto = true;
static char s_latest[16] = "", s_status[112] = "waiting for internet";
static char s_snd[24] = "";                 // sound bank version the manifest offers (hash of sounds.bin)
static int s_snd_size;
static char s_snd_tried[24] = "";           // last version tried, and when
static int64_t s_snd_tried_at;
static char s_snd_status[72] = "waiting for internet";

static void snd_status(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_snd_status, sizeof s_snd_status, fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "voice bank: %s", s_snd_status);
}

void update_sound_status(char *out, size_t n) { strlcpy(out, s_snd_status, n); }

static void status(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_status, sizeof s_status, fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", s_status);
}

static void nvs_str(const char *key, char *val, size_t n, bool write)
{
    nvs_handle_t h;
    if (nvs_open("upd", write ? NVS_READWRITE : NVS_READONLY, &h) != ESP_OK) { if (!write) val[0] = 0; return; }
    if (write) { nvs_set_str(h, key, val); nvs_commit(h); }
    else if (nvs_get_str(h, key, val, &n) != ESP_OK) val[0] = 0;
    nvs_close(h);
}

// GET <base>manifest.json -> "version". Query string busts the GitHub Pages CDN cache (~10 min).
static char s_why[48];       // why the last check failed, shown in the app

static bool fetch_latest(char *ver, size_t n)
{
    char url[160];
    snprintf(url, sizeof url, "%smanifest.json?t=%lld", CONFIG_XARM_UPDATE_URL, esp_timer_get_time());
    esp_http_client_config_t c = { .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000 };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    bool ok = false;
    const int cap = 4096;
    char *buf = malloc(cap);
    esp_err_t oe = ESP_FAIL;
    int code = 0;
    if (buf && (oe = esp_http_client_open(h, 0)) == ESP_OK && esp_http_client_fetch_headers(h) >= 0
        && (code = esp_http_client_get_status_code(h)) == 200) {
        int len = 0, r;
        while (len < cap - 1 && (r = esp_http_client_read(h, buf + len, cap - 1 - len)) > 0) len += r;
        buf[len] = 0;
        cJSON *j = cJSON_Parse(buf);
        cJSON *v = cJSON_GetObjectItem(j, "version");
        if (cJSON_IsString(v)) { strlcpy(ver, v->valuestring, n); ok = true; }
        cJSON *sv = cJSON_GetObjectItem(j, "sounds"), *ss = cJSON_GetObjectItem(j, "sounds_size");
        if (cJSON_IsString(sv) && cJSON_IsNumber(ss)) { strlcpy(s_snd, sv->valuestring, sizeof s_snd); s_snd_size = ss->valueint; }
        cJSON_Delete(j);
    }
    if (!ok) snprintf(s_why, sizeof s_why, "%s, %u KB free", oe != ESP_OK ? esp_err_to_name(oe) : code ? "bad reply" : "no reply",
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    free(buf);
    esp_http_client_cleanup(h);
    return ok;
}

static void install(const char *ver)
{
    char url[160];
    snprintf(url, sizeof url, "%sxarm-voice.bin?v=%s", CONFIG_XARM_UPDATE_URL, ver);
    esp_http_client_config_t hc = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000,
        .buffer_size = 4096, .buffer_size_tx = 1024, .keep_alive_enable = true,
    };
    esp_https_ota_config_t oc = { .http_config = &hc };
    pick_stop();
    status("installing %s…", ver);
    fx_state("updating");
    esp_err_t e = esp_https_ota(&oc);              // writes the idle slot + sets it to boot
    if (e == ESP_OK) {
        nvs_str("tried", (char *)ver, 0, true);     // one attempt per version: a build that rolls back isn't retried
        status("installed %s — rebooting", ver);
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    status("install of %s failed: %s", ver, esp_err_to_name(e));
    fx_event(FX_ERROR);
}

// The sound bank (voice + bells, tools/make_sounds.py) lives in the "model" partition after the speech
// models, not in the app, so it's synced on its own: download sounds.bin into place, then reload.
// No reboot. Sounds go quiet for the ~30 s it takes.
static bool sync_sounds(void)
{
    char have[24];
    nvs_str("sndver", have, sizeof have, false);
    int64_t now = esp_timer_get_time();
    bool retry_ok = strcmp(s_snd_tried, s_snd) || now - s_snd_tried_at > 10LL * 60 * 1000000;   // a failed try: again in 10 min
    bool want = s_snd[0] && s_snd_size > 0 && s_snd_size <= FX_BANK_MAX && (strcmp(have, s_snd) || !fx_bank_ok());
    if (!want) { if (fx_bank_ok()) snd_status("ready (%s)", have); return true; }
    if (!retry_ok) return false;
    strlcpy(s_snd_tried, s_snd, sizeof s_snd_tried);
    s_snd_tried_at = now;
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
    if (!p || p->size < fx_bank_off(p->size) + FX_BANK_MAX) { snd_status("no room in flash for the voice"); return false; }
    const uint32_t off = fx_bank_off(p->size);

    char url[160];
    snprintf(url, sizeof url, "%ssounds.bin?v=%s", CONFIG_XARM_UPDATE_URL, s_snd);
    esp_http_client_config_t c = { .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 20000,
                                   .buffer_size = 4096, .keep_alive_enable = true };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    uint8_t *buf = malloc(4096);
    bool ok = false;
    int got = 0;
    if (buf && esp_http_client_open(h, 0) == ESP_OK && esp_http_client_fetch_headers(h) >= 0
        && esp_http_client_get_status_code(h) == 200) {
        snd_status("downloading Muffin's voice…");
        fx_bank_release();
        // erase 64 KB at a time just ahead of the data. Erasing the whole 2-3 MB first took ~20 s, long
        // enough for the server to drop the idle connection, so the download died every time.
        uint32_t erased = 0;
        int r = 0;
        while (got < s_snd_size && (r = esp_http_client_read(h, (char *)buf, 4096)) > 0) {
            if (got + r > s_snd_size) r = s_snd_size - got;
            while ((uint32_t)(got + r) > erased) {
                if (esp_partition_erase_range(p, off + erased, 0x10000) != ESP_OK) { r = -1; break; }
                erased += 0x10000;
            }
            if (r < 0 || esp_partition_write(p, off + got, buf, r) != ESP_OK) break;
            got += r;
            if ((got & 0x3FFFF) < 4096) snd_status("downloading Muffin's voice… %d%%", (int)(got * 100LL / s_snd_size));
        }
        char magic[4] = {0};
        esp_partition_read(p, off, magic, 4);
        ok = got == s_snd_size && !memcmp(magic, "SNB2", 4);
        fx_bank_reload();
    }
    free(buf);
    esp_http_client_cleanup(h);
    if (ok) {
        nvs_str("sndver", s_snd, 0, true);
        snd_status("ready (%s)", s_snd);
        fx_say(SND_UPDATED);
    } else {
        snd_status("download failed at %d of %d bytes — retrying in 10 min", got, s_snd_size);
    }
    return ok;
}

static void task(void *arg)
{
    const char *running = esp_app_get_description()->version;
    for (;;) {
        if (!net_online()) {
            status("no internet — set home WiFi to auto-update");
        } else {
            char latest[16];
            if (!fetch_latest(latest, sizeof latest)) {
                status("couldn't reach update server (%s)", s_why);
            } else {
                strlcpy(s_latest, latest, sizeof s_latest);
                // sounds first: they don't reboot, and they belong to the same build as the app on offer
                if (!pick_busy()) sync_sounds();
                char tried[16];
                nvs_str("tried", tried, sizeof tried, false);
                if (!strcmp(latest, running))            status("up to date (%s)", running);
                else if (!s_auto)                         status("%s available — auto-update off", latest);
                else if (!strcmp(latest, tried))          status("%s didn't stick (rolled back?) — not retrying", latest);
                else if (pick_busy())                     status("%s available — waiting for the arm to finish", latest);
                else install(latest);
            }
        }
        // sleep until the next check, or until update_check_now()
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONFIG_XARM_UPDATE_EVERY_S * 1000));
    }
}

bool update_new_version(void)
{
    const char *running = esp_app_get_description()->version;
    char last[33];
    nvs_str("lastver", last, sizeof last, false);
    if (!strcmp(last, running)) return false;
    nvs_str("lastver", (char *)running, 0, true);
    return true;
}

void update_start(void)
{
    char a[4];
    nvs_str("auto", a, sizeof a, false);
    s_auto = a[0] != '0';                              // default on
    xTaskCreate(task, "update", 8192, NULL, 3, &s_task);
}

void update_check_now(void) { if (s_task) xTaskNotifyGive(s_task); }

void update_set_auto(bool on)
{
    s_auto = on;
    char a[2] = {on ? '1' : '0', 0};
    nvs_str("auto", a, 0, true);
    update_check_now();
}

bool update_auto(void) { return s_auto; }

void update_status(char *latest, size_t nl, char *st, size_t ns)
{
    strlcpy(latest, s_latest, nl);
    strlcpy(st, s_status, ns);
}
