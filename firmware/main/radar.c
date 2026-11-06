// radar.c — see radar.h. Starts by counting edges on the pin (1 s timer + interrupt). If the pin turns out to carry
// UART data, it switches to UART2 RX on that pin, finds the baud rate, and reads Hi-Link frames:
//   LD2410-style binary reports  F4 F3 F2 F1 <len16> ... F8 F7 F6 F5   (target state + moving/still distance)
//   text modules (LD2420/LD2402)  "ON" / "OFF" / "Range 123" / "distance:123"
// Anything else is kept as a raw sample in /api/state so the format can be read off.
#include "radar.h"
#include "sdkconfig.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#ifndef CONFIG_XARM_RADAR_GPIO
#define CONFIG_XARM_RADAR_GPIO -1
#endif
#define PIN CONFIG_XARM_RADAR_GPIO
#define UARTN UART_NUM_2
#define SERIAL_EDGES 200           // edges per second above this: the pin is carrying UART data

static const char *TAG = "radar";
static volatile uint32_t s_edges, s_rate;
static volatile bool s_level, s_present, s_serial;
static volatile int64_t s_since_us, s_frame_us;
static volatile int s_baud, s_dist_cm = -1, s_frames;
static char s_fmt[12] = "", s_raw[100] = "";
static esp_timer_handle_t s_tmr;
static bool s_ok;

static void set_present(bool p)
{
    if (p == s_present) return;
    s_present = p;
    s_since_us = esp_timer_get_time();
    ESP_LOGI(TAG, "%s", p ? "someone's here" : "nobody");
}

// ---------------------------------------------------------------- serial: parsing
static void raw_sample(const uint8_t *b, int n)
{
    size_t k = 0;
    for (int i = 0; i < n && k + 4 < sizeof s_raw; i++) k += snprintf(s_raw + k, sizeof s_raw - k, "%02X ", b[i]);
    if (k) s_raw[k - 1] = 0;
}

static bool parse_ld2410(const uint8_t *b, int n)
{
    for (int i = 0; i + 12 <= n; i++) {
        if (b[i] != 0xF4 || b[i + 1] != 0xF3 || b[i + 2] != 0xF2 || b[i + 3] != 0xF1) continue;
        int len = b[i + 4] | b[i + 5] << 8;
        if (len < 4 || i + 10 + len > n) return false;
        const uint8_t *d = b + i + 6;          // [type][0xAA][state][move cm lo hi][move energy][still cm lo hi]...
        if (d[1] != 0xAA) continue;
        int state = d[2];                      // 0 nobody, 1 moving, 2 still, 3 both
        int mv = d[3] | d[4] << 8, st = len >= 9 ? (d[6] | d[7] << 8) : 0;
        set_present(state != 0);
        s_dist_cm = state == 0 ? -1 : (state == 2 ? st : mv);
        strlcpy(s_fmt, "ld2410", sizeof s_fmt);
        s_frame_us = esp_timer_get_time();
        s_frames++;
        return true;
    }
    return false;
}

static bool parse_text(const uint8_t *b, int n)
{
    static char line[48];
    static int k;
    bool any = false;
    for (int i = 0; i < n; i++) {
        char c = (char)b[i];
        if (c != '\n' && c != '\r') { if (k < (int)sizeof line - 1 && isprint((unsigned char)c)) line[k++] = c; continue; }
        if (!k) continue;
        line[k] = 0; k = 0;
        int v;
        if (!strcmp(line, "ON")) set_present(true);
        else if (!strcmp(line, "OFF")) { set_present(false); s_dist_cm = -1; }
        else if (sscanf(line, "Range %d", &v) == 1 || sscanf(line, "distance:%d", &v) == 1) { set_present(true); s_dist_cm = v; }
        else continue;
        strlcpy(s_fmt, "text", sizeof s_fmt);
        s_frame_us = esp_timer_get_time();
        s_frames++;
        any = true;
    }
    return any;
}

// how much of a buffer looks like a known format at this baud (0..100)
static int score(const uint8_t *b, int n)
{
    if (n < 8) return 0;
    for (int i = 0; i + 4 <= n; i++) if (b[i] == 0xF4 && b[i + 1] == 0xF3 && b[i + 2] == 0xF2 && b[i + 3] == 0xF1) return 100;
    int ok = 0;
    for (int i = 0; i < n; i++) ok += isprint(b[i]) || b[i] == '\r' || b[i] == '\n';
    return ok * 90 / n;
}

static void serial_task(void *arg)
{
    static uint8_t buf[256];
    const int bauds[] = {256000, 115200, 57600, 9600};
    int best = 0, best_s = -1;
    for (int t = 0; t < 2 && best_s < 80; t++)
        for (size_t i = 0; i < sizeof bauds / sizeof *bauds; i++) {
            uart_set_baudrate(UARTN, bauds[i]);
            uart_flush_input(UARTN);
            vTaskDelay(pdMS_TO_TICKS(30));
            int n = uart_read_bytes(UARTN, buf, sizeof buf, pdMS_TO_TICKS(600));
            int s = score(buf, n);
            ESP_LOGI(TAG, "baud %d: %d bytes, looks %d%% right", bauds[i], n, s);
            if (s > best_s) { best_s = s; best = bauds[i]; raw_sample(buf, n < 32 ? n : 32); }
        }
    s_baud = best;
    uart_set_baudrate(UARTN, best);
    uart_flush_input(UARTN);
    ESP_LOGI(TAG, "reading the radar at %d baud", best);
    for (;;) {
        int n = uart_read_bytes(UARTN, buf, sizeof buf, pdMS_TO_TICKS(200));
        if (n <= 0) continue;
        if (!parse_ld2410(buf, n) && !parse_text(buf, n)) raw_sample(buf, n < 32 ? n : 32);
        if (s_frame_us && esp_timer_get_time() - s_frame_us > 3000000) set_present(false);   // went quiet
    }
}

static void go_serial(void)
{
    gpio_isr_handler_remove(PIN);
    gpio_set_intr_type(PIN, GPIO_INTR_DISABLE);
    uart_config_t c = { .baud_rate = 256000, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    if (uart_driver_install(UARTN, 512, 0, 0, NULL, 0) != ESP_OK) { ESP_LOGW(TAG, "no UART for the radar"); return; }
    uart_param_config(UARTN, &c);
    uart_set_pin(UARTN, UART_PIN_NO_CHANGE, PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);   // RX only
    // the task only reads the UART: no flash writes, so a PSRAM stack is fine (internal RAM is nearly gone)
    if (xTaskCreatePinnedToCoreWithCaps(serial_task, "radar", 3072, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        ESP_LOGW(TAG, "no memory for the radar task");
    s_serial = true;
}

// ---------------------------------------------------------------- edge counting (first, to tell what the pin does)
static void IRAM_ATTR on_edge(void *arg) { s_edges++; }

static void every_second(void *arg)
{
    uint32_t e = s_edges;
    s_edges = 0;
    s_rate = e;
    if (s_serial) return;
    bool lv = gpio_get_level(PIN);
    s_level = lv;
    if (e >= SERIAL_EDGES) { esp_timer_stop(s_tmr); go_serial(); return; }   // it's data, not a level: read it
    set_present(lv);
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
    if (esp_timer_create(&t, &s_tmr) != ESP_OK) return;
    esp_timer_start_periodic(s_tmr, 1000000);
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
    cJSON_AddStringToObject(r, "kind", s_serial ? "serial" : "presence");
    cJSON_AddBoolToObject(r, "present", s_present);
    cJSON_AddNumberToObject(r, "since_s", (double)((esp_timer_get_time() - s_since_us) / 1000000));
    if (s_serial) {
        cJSON_AddNumberToObject(r, "baud", s_baud);
        cJSON_AddStringToObject(r, "format", s_fmt[0] ? s_fmt : (s_baud ? "unknown" : "finding baud"));
        cJSON_AddNumberToObject(r, "frames", s_frames);
        cJSON_AddNumberToObject(r, "distance_cm", s_dist_cm);
        cJSON_AddStringToObject(r, "raw", s_raw);
    } else {
        cJSON_AddBoolToObject(r, "level", s_level);
        cJSON_AddNumberToObject(r, "edges_s", s_rate);
    }
}
