// radar.c — see radar.h. One input pin, an edge-counting interrupt and a 1 s timer: no task, ~nothing in RAM.
#include "radar.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "driver/gpio.h"

#ifndef CONFIG_XARM_RADAR_GPIO
#define CONFIG_XARM_RADAR_GPIO -1
#endif
#define PIN CONFIG_XARM_RADAR_GPIO
#define SERIAL_EDGES 200           // edges per second above this: the pin is carrying UART data

static const char *TAG = "radar";
static volatile uint32_t s_edges;
static volatile uint32_t s_rate;
static volatile bool s_level, s_present;
static volatile int64_t s_since_us;
static bool s_ok;

static void IRAM_ATTR on_edge(void *arg) { s_edges++; }

static void every_second(void *arg)
{
    uint32_t e = s_edges;
    s_edges = 0;
    s_rate = e;
    bool lv = gpio_get_level(PIN);
    s_level = lv;
    bool p = lv && e < SERIAL_EDGES;
    if (p != s_present) {
        s_present = p;
        s_since_us = esp_timer_get_time();
        ESP_LOGI(TAG, "%s", p ? "someone's here" : "nobody");
    }
}

void radar_init(void)
{
    if (PIN < 0) return;
    gpio_config_t c = { .pin_bit_mask = 1ULL << PIN, .mode = GPIO_MODE_INPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE,
                        .intr_type = GPIO_INTR_ANYEDGE };
    if (gpio_config(&c) != ESP_OK) return;
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { ESP_LOGW(TAG, "isr service: %s", esp_err_to_name(e)); return; }
    gpio_isr_handler_add(PIN, on_edge, NULL);
    const esp_timer_create_args_t t = { .callback = every_second, .name = "radar" };
    esp_timer_handle_t h;
    if (esp_timer_create(&t, &h) != ESP_OK) return;
    esp_timer_start_periodic(h, 1000000);
    s_since_us = esp_timer_get_time();
    s_ok = true;
    ESP_LOGI(TAG, "radar on GPIO%d", PIN);
}

bool radar_present(void) { return s_ok && s_present; }

void radar_json(cJSON *o)
{
    cJSON *r = cJSON_AddObjectToObject(o, "radar");
    cJSON_AddNumberToObject(r, "gpio", PIN);
    if (!s_ok) { cJSON_AddStringToObject(r, "kind", "none"); return; }
    cJSON_AddBoolToObject(r, "level", s_level);
    cJSON_AddNumberToObject(r, "edges_s", s_rate);
    cJSON_AddStringToObject(r, "kind", s_rate >= SERIAL_EDGES ? "serial" : "presence");
    cJSON_AddBoolToObject(r, "present", s_present);
    cJSON_AddNumberToObject(r, "since_s", (double)((esp_timer_get_time() - s_since_us) / 1000000));
}
