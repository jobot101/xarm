#include "update.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "pick.h"
#include "sdkconfig.h"

static const char *TAG = "update";
static TaskHandle_t s_task;
static bool s_auto = true;
static char s_latest[16] = "", s_status[64] = "waiting for internet";

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
static bool fetch_latest(char *ver, size_t n)
{
    char url[160];
    snprintf(url, sizeof url, "%smanifest.json?t=%lld", CONFIG_XARM_UPDATE_URL, esp_timer_get_time());
    esp_http_client_config_t c = { .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000 };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    bool ok = false;
    char buf[1024];
    if (esp_http_client_open(h, 0) == ESP_OK && esp_http_client_fetch_headers(h) >= 0
        && esp_http_client_get_status_code(h) == 200) {
        int len = 0, r;
        while (len < (int)sizeof buf - 1 && (r = esp_http_client_read(h, buf + len, sizeof buf - 1 - len)) > 0) len += r;
        buf[len] = 0;
        cJSON *j = cJSON_Parse(buf);
        cJSON *v = cJSON_GetObjectItem(j, "version");
        if (cJSON_IsString(v)) { strlcpy(ver, v->valuestring, n); ok = true; }
        cJSON_Delete(j);
    }
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
    esp_err_t e = esp_https_ota(&oc);              // writes the idle slot + sets it to boot
    if (e == ESP_OK) {
        nvs_str("tried", (char *)ver, 0, true);     // one attempt per version: a build that rolls back isn't retried
        status("installed %s — rebooting", ver);
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    status("install of %s failed: %s", ver, esp_err_to_name(e));
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
                status("couldn't reach update server");
            } else {
                strlcpy(s_latest, latest, sizeof s_latest);
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
