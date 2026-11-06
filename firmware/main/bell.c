// bell.c — the solenoid striker. See bell.h.
#include "bell.h"
#include "sdkconfig.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "voice.h"

#ifndef CONFIG_XARM_BELL_GPIO
#define CONFIG_XARM_BELL_GPIO -1
#endif
#define PIN CONFIG_XARM_BELL_GPIO
#define MS_MIN 3
#define MS_MAX 30                  // taps only: a long pulse drives the plunger its full travel (can wreck the bell)
#define REST_MS 120                // between pulses: lets the plunger fall back and the coil cool

static const char *TAG = "bell";
static SemaphoreHandle_t s_lock;
static int s_ms = 12;
static int64_t s_last_us;

bool bell_ready(void) { return PIN >= 0 && s_lock; }
static void load(void);
int bell_ms(void) { load(); return s_ms; }

void bell_init(void)
{
    if (PIN < 0) return;
    gpio_config_t c = { .pin_bit_mask = 1ULL << PIN, .mode = GPIO_MODE_OUTPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE };
    gpio_set_level(PIN, 0);
    gpio_config(&c);
    gpio_set_level(PIN, 0);
    s_lock = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "solenoid on GPIO%d, held low", PIN);
}

// the saved pulse, read on first use: bell_init() runs before NVS is up
static void load(void)
{
    static bool done;
    if (done) return;
    done = true;
    nvs_handle_t h;
    uint8_t v;
    if (nvs_open("xarm", NVS_READONLY, &h) == ESP_OK) { if (nvs_get_u8(h, "bell_ms", &v) == ESP_OK && v) s_ms = v > MS_MAX ? MS_MAX : v; nvs_close(h); }
}

static void save_ms(int ms)
{
    s_ms = ms;
    nvs_handle_t h;
    if (nvs_open("xarm", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "bell_ms", (uint8_t)ms); nvs_commit(h); nvs_close(h); }
}

static int clamp_ms(int ms) { load(); if (ms <= 0) ms = s_ms; return ms < MS_MIN ? MS_MIN : ms > MS_MAX ? MS_MAX : ms; }

void bell_strike(int ms)
{
    if (!bell_ready()) return;
    ms = clamp_ms(ms);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t rest = s_last_us + (int64_t)REST_MS * 1000 - esp_timer_get_time();
    if (rest > 0) vTaskDelay(pdMS_TO_TICKS(rest / 1000 + 1));
    gpio_set_level(PIN, 1);
    int64_t t = esp_timer_get_time();
    while (esp_timer_get_time() - t < (int64_t)ms * 1000) ;          // busy-wait: exact length, at most 30 ms
    gpio_set_level(PIN, 0);
    s_last_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

static esp_err_t h_bell(httpd_req_t *r)
{
    char body[160] = "";
    int n = r->content_len < (int)sizeof body - 1 ? r->content_len : (int)sizeof body - 1, got = 0;
    while (got < n) { int k = httpd_req_recv(r, body + got, n - got); if (k <= 0) return ESP_FAIL; got += k; }
    body[got] = 0;
    cJSON *j = cJSON_Parse(body), *v;
    int ms = clamp_ms(j && cJSON_IsNumber(v = cJSON_GetObjectItem(j, "ms")) ? v->valueint : 0);
    int cnt = j && cJSON_IsNumber(v = cJSON_GetObjectItem(j, "n")) ? v->valueint : 1;
    int gap = j && cJSON_IsNumber(v = cJSON_GetObjectItem(j, "gap")) ? v->valueint : 600;
    bool save = j && cJSON_IsTrue(cJSON_GetObjectItem(j, "save"));
    bool listen = !j || !cJSON_IsFalse(cJSON_GetObjectItem(j, "listen"));
    cJSON_Delete(j);
    // this runs on the web server's only task: keep it short (the PC websocket and every request wait behind it)
    if (cnt < 1) cnt = 1;
    if (cnt > 3) cnt = 3;
    if (gap < 150) gap = 150;
    if (gap > 600) gap = 600;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", bell_ready());
    cJSON_AddNumberToObject(o, "gpio", PIN);
    cJSON_AddNumberToObject(o, "ms", ms);
    if (bell_ready()) {
        if (save) save_ms(ms);
        int64_t t0 = esp_timer_get_time();
        for (int i = 0; i < cnt; i++) { if (i) vTaskDelay(pdMS_TO_TICKS(gap)); bell_strike(ms); }
        if (listen) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            static float l1[200], l2[200];
            int pre = 0, k = voice_levels_since(t0, l1, l2, 200, &pre);
            cJSON_AddNumberToObject(o, "hop_ms", 16);
            cJSON_AddNumberToObject(o, "pre", pre);                 // hops before the first strike (the room)
            cJSON *a = cJSON_AddArrayToObject(o, "mic1"), *b = cJSON_AddArrayToObject(o, "mic2");
            for (int i = 0; i < k; i++) {
                cJSON_AddItemToArray(a, cJSON_CreateNumber(roundf(l1[i] * 10) / 10));
                cJSON_AddItemToArray(b, cJSON_CreateNumber(roundf(l2[i] * 10) / 10));
            }
        }
    }
    cJSON_AddNumberToObject(o, "saved_ms", bell_ms());
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    httpd_resp_set_type(r, "application/json");
    esp_err_t e = httpd_resp_sendstr(r, txt ? txt : "{}");
    free(txt);
    return e;
}

void bell_register(httpd_handle_t h)
{
    httpd_uri_t u = { .uri = "/api/bell", .method = HTTP_POST, .handler = h_bell };
    httpd_register_uri_handler(h, &u);
}
