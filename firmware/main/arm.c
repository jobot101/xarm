#include "arm.h"
#include <string.h>
#include "driver/uart.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define UART UART_NUM_1
static const char *TAG = "arm";
static const uint8_t IDS[NSERVO] = {1, 2, 3, 4, 5, 6};

static SemaphoreHandle_t s_tx, s_txn, s_read_done;
static uint16_t s_last[NSERVO] = {500, 500, 500, 500, 500, 500};
static uint16_t s_rd[NSERVO];
static volatile int s_batt;
static volatile bool s_torque = true;
static volatile int64_t s_last_rx_us;
#define UNREAD 0xFFFF
// soft limits per servo (index 0 gripper .. 5 base). Every move is clamped here — one choke point.
static uint16_t s_lim[NSERVO][2] = {{100, 800}, {50, 950}, {80, 920}, {80, 920}, {80, 920}, {0, 1000}};

static uint16_t lim(int i, int v)
{
    if (v < s_lim[i][0]) return s_lim[i][0];
    if (v > s_lim[i][1]) return s_lim[i][1];
    return (uint16_t)v;
}

void arm_limits(uint16_t out[NSERVO][2]) { memcpy(out, s_lim, sizeof s_lim); }

void arm_limits_set(int i, int lo, int hi)
{
    if (i < 0 || i >= NSERVO) return;
    lo = clamp_pos(lo); hi = clamp_pos(hi);
    if (hi - lo < 20) return;                  // refuse a nonsense range
    s_lim[i][0] = lo; s_lim[i][1] = hi;
    nvs_handle_t h;
    if (nvs_open("xarm", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "limits", s_lim, sizeof s_lim);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void limits_load(void)
{
    nvs_handle_t h;
    if (nvs_open("xarm", NVS_READONLY, &h) != ESP_OK) return;
    uint16_t l[NSERVO][2]; size_t n = sizeof l;
    if (nvs_get_blob(h, "limits", l, &n) == ESP_OK && n == sizeof l) memcpy(s_lim, l, sizeof l);
    nvs_close(h);
}

static void tx(const uint8_t *b, size_t n)
{
    xSemaphoreTake(s_tx, portMAX_DELAY);
    uart_write_bytes(UART, b, n);
    xSemaphoreGive(s_tx);
}

static void rx_task(void *arg)
{
    xparser_t p = {0};
    uint8_t buf[64];
    for (;;) {
        int n = uart_read_bytes(UART, buf, sizeof buf, pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) {
            if (!xp_feed(&p, buf[i])) continue;
            s_last_rx_us = esp_timer_get_time();
            if (p.cmd == XCMD_POS_READ) {
                if (xp_positions(&p, s_rd) > 0) xSemaphoreGive(s_read_done);
            } else if (p.cmd == XCMD_BATTERY && p.np >= 2) {
                s_batt = p.p[0] | (p.p[1] << 8);
            }
        }
    }
}

static void poll_task(void *arg)
{
    uint8_t f[8];
    int tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(300));
        if (!s_torque) {                       // limp: stream real positions so the app sliders follow your hand
            uint16_t p[NSERVO];
            if (arm_read(p)) memcpy(s_last, p, sizeof p);
        }
        if (++tick % 10 == 0) tx(f, xf_simple(f, XCMD_BATTERY));
    }
}

void arm_init(void)
{
    s_tx = xSemaphoreCreateMutex();
    s_txn = xSemaphoreCreateMutex();
    s_read_done = xSemaphoreCreateBinary();
    limits_load();
    uart_config_t c = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART, 512, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART, &c));
    ESP_ERROR_CHECK(uart_set_pin(UART, CONFIG_XARM_UART_TX, CONFIG_XARM_UART_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreate(rx_task, "arm_rx", 3072, NULL, 6, NULL);
    xTaskCreate(poll_task, "arm_poll", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "uart tx=%d rx=%d", CONFIG_XARM_UART_TX, CONFIG_XARM_UART_RX);
}

void arm_move(const uint16_t pos[NSERVO], uint16_t ms)
{
    uint8_t f[32];
    uint16_t p[NSERVO];
    for (int i = 0; i < NSERVO; i++) p[i] = lim(i, pos[i]);
    tx(f, xf_move(f, IDS, p, NSERVO, ms));
    memcpy(s_last, p, sizeof p);
    s_torque = true;                           // any move re-energizes the servos
}

void arm_move_one(int id, uint16_t pos, uint16_t ms)
{
    if (id < 1 || id > NSERVO) return;
    uint8_t f[16], i = (uint8_t)id;
    pos = lim(id - 1, pos);
    tx(f, xf_move(f, &i, &pos, 1, ms));
    s_last[id - 1] = pos;
}

bool arm_read(uint16_t out[NSERVO])
{
    uint8_t f[16];
    xSemaphoreTake(s_txn, portMAX_DELAY);
    xSemaphoreTake(s_read_done, 0);            // drop a stale reply
    for (int i = 0; i < NSERVO; i++) s_rd[i] = UNREAD;
    tx(f, xf_ids(f, XCMD_POS_READ, IDS, NSERVO));
    bool ok = xSemaphoreTake(s_read_done, pdMS_TO_TICKS(400)) == pdTRUE;
    if (ok) for (int i = 0; i < NSERVO; i++) {
        // a servo that didn't answer, or reports 0 (the controller's "couldn't read"), keeps its last known spot
        bool bad = s_rd[i] == UNREAD || s_rd[i] == 0;
        out[i] = bad ? s_last[i] : s_rd[i];
        if (bad) ESP_LOGD(TAG, "servo %d read failed, keeping %d", i + 1, s_last[i]);
    }
    xSemaphoreGive(s_txn);
    return ok;
}

void arm_torque(bool on)
{
    if (on) { arm_hold(); return; }
    uint8_t f[16];
    tx(f, xf_ids(f, XCMD_UNLOAD, IDS, NSERVO));
    s_torque = false;
}

void arm_hold(void)
{
    uint16_t p[NSERVO];
    if (s_torque || !arm_read(p)) memcpy(p, s_last, sizeof p);   // powered: commanded = actual; limp: read where the hand left it
    arm_move(p, 80);
}

bool arm_torque_on(void) { return s_torque; }
void arm_last(uint16_t out[NSERVO]) { memcpy(out, s_last, sizeof s_last); }
int  arm_battery_mv(void) { return s_batt; }
bool arm_link_ok(void) { return s_last_rx_us && esp_timer_get_time() - s_last_rx_us < 5000000; }
