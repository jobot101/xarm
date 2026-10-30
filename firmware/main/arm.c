#include "arm.h"
#include <math.h>
#include <string.h>
#include "driver/uart.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

// ---- motion streamer ----
// The controller only knows "go to P in T ms": a straight line at constant speed with a hard start and
// stop. So nothing here sends a whole move at once. A 25 Hz task walks every joint along a smooth
// curve and sends the next setpoint each tick with T a bit longer than the tick, so the servos are
// always mid-move when the next frame lands and never stop between frames.
#define TICK_MS     40        // 25 Hz: a 6-servo frame is 25 bytes = 26 ms at 9600 baud
#define SERVO_MS    60        // > TICK_MS: overlap = no stop-go. Servos trail the curve by ~60 ms
#define VMAX_FOLLOW 1200.0f   // slider chase, units/s (servo top speed is ~1500)
#define AMAX_FOLLOW 5000.0f   // units/s^2
#define AMAX_DRIVE  3000.0f   // drive pad ramp
#define AMAX_STOP   8000.0f   // "stop": 500 units/s to rest in ~60 ms

#define UART UART_NUM_1
static const char *TAG = "arm";
static const uint8_t IDS[NSERVO] = {1, 2, 3, 4, 5, 6};

typedef enum { J_HOLD, J_EASE, J_LIN, J_FOLLOW, J_VEL } jmode_t;
typedef struct {
    jmode_t m;
    float cur, vel;           // setpoint being streamed, and its velocity (units/s)
    float from, to, vdes;
    int64_t t0;
    int dur_us;
    bool brake;
    int sent;                 // last value on the wire, -1 = resend
} joint_t;

static SemaphoreHandle_t s_tx, s_txn, s_read_done, s_mx;
static joint_t s_j[NSERVO];
static volatile int64_t s_manual_us;         // last jog / drive / torque change (the app's hands on the arm)
static int64_t s_drive_until;
static uint16_t s_rd[NSERVO];
static volatile int s_batt;
static volatile bool s_torque = true;
static volatile int64_t s_last_rx_us;
#define UNREAD 0xFFFF
// soft limits per servo (index 0 gripper .. 5 base). Every setpoint is clamped here — one choke point.
static uint16_t s_lim[NSERVO][2] = {{100, 800}, {50, 950}, {80, 920}, {80, 920}, {80, 920}, {0, 1000}};

static float limf(int i, float v)
{
    if (v < s_lim[i][0]) return s_lim[i][0];
    if (v > s_lim[i][1]) return s_lim[i][1];
    return v;
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

static void lock(void)   { xSemaphoreTake(s_mx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mx); }

// set where the servos really are (after limp / at boot): no motion, resend next tick
static void place(const uint16_t p[NSERVO])
{
    for (int i = 0; i < NSERVO; i++) {
        joint_t *j = &s_j[i];
        j->m = J_HOLD; j->cur = j->from = j->to = p[i]; j->vel = j->vdes = 0; j->sent = -1;
    }
}

// one joint's next setpoint
static void step(int i, int64_t now, float dt)
{
    joint_t *j = &s_j[i];
    float prev = j->cur;
    switch (j->m) {
    case J_HOLD:
        j->vel = 0;
        return;
    case J_EASE:
    case J_LIN: {
        float s = j->dur_us > 0 ? (float)(now - j->t0) / j->dur_us : 1;
        if (s >= 1) { j->cur = j->to; j->m = J_HOLD; }
        else j->cur = j->from + (j->to - j->from) * (j->m == J_EASE ? ease_minjerk(s) : s);
        j->vel = (j->cur - prev) / dt;
        break;
    }
    case J_FOLLOW:
        if (follow_step(&j->cur, &j->vel, j->to, VMAX_FOLLOW, AMAX_FOLLOW, dt)) j->m = J_HOLD;
        break;
    case J_VEL:
        vel_step(&j->cur, &j->vel, j->vdes, j->brake ? AMAX_STOP : AMAX_DRIVE, dt);
        if (j->vdes == 0 && j->vel == 0) { j->m = J_HOLD; j->brake = false; }
        break;
    }
    float c = limf(i, j->cur);
    if (c != j->cur) {                         // ran into a soft limit: stop there
        j->cur = c; j->vel = 0;
        if (j->m == J_VEL) j->vdes = 0;
    }
}

static void stream_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    const float dt = TICK_MS / 1000.0f;
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(TICK_MS));
        uint8_t ids[NSERVO]; uint16_t val[NSERVO]; int n = 0;
        lock();
        if (!s_torque) { unlock(); continue; }   // limp: sending anything would re-energize
        int64_t now = esp_timer_get_time();
        if (s_drive_until && now > s_drive_until) {      // drive pad went quiet (lost packet): coast down
            for (int i = 0; i < NSERVO; i++) if (s_j[i].m == J_VEL) s_j[i].vdes = 0;
            s_drive_until = 0;
        }
        for (int i = 0; i < NSERVO; i++) {
            step(i, now, dt);
            int v = (int)lroundf(s_j[i].cur);
            if (v != s_j[i].sent) { ids[n] = IDS[i]; val[n] = (uint16_t)v; s_j[i].sent = v; n++; }
        }
        // sent under the lock so a frame can't slip out after arm_torque(false)'s unload
        if (n) { uint8_t f[32]; tx(f, xf_move(f, ids, val, n, SERVO_MS)); }
        unlock();
    }
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
            if (arm_read(p) && !s_torque) { lock(); place(p); unlock(); }
        }
        if (++tick % 10 == 0) tx(f, xf_simple(f, XCMD_BATTERY));
    }
}

void arm_init(void)
{
    s_tx = xSemaphoreCreateMutex();
    s_txn = xSemaphoreCreateMutex();
    s_mx = xSemaphoreCreateMutex();
    s_read_done = xSemaphoreCreateBinary();
    limits_load();
    uint16_t mid[NSERVO] = {500, 500, 500, 500, 500, 500};
    place(mid);
    for (int i = 0; i < NSERVO; i++) s_j[i].sent = mid[i];   // don't yank the arm to center at boot
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
    // start from where the arm really is, so the first move glides instead of jumping from 500
    uint16_t p[NSERVO];
    if (arm_read(p)) { lock(); place(p); for (int i = 0; i < NSERVO; i++) s_j[i].sent = p[i]; unlock(); }
    xTaskCreatePinnedToCore(stream_task, "arm_stream", 3072, NULL, 7, NULL, 0);
    ESP_LOGI(TAG, "uart tx=%d rx=%d, streaming %d Hz", CONFIG_XARM_UART_TX, CONFIG_XARM_UART_RX, 1000 / TICK_MS);
}

static void plan(int i, uint16_t pos, uint16_t ms, jmode_t m, int64_t now)
{
    joint_t *j = &s_j[i];
    j->from = j->cur;
    j->to = limf(i, pos);
    j->t0 = now;
    j->dur_us = ms * 1000;
    j->m = ms ? m : J_FOLLOW;                  // 0 ms = "as fast as is smooth"
    j->brake = false;
}

static void move_all(const uint16_t pos[NSERVO], uint16_t ms, jmode_t m)
{
    lock();
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < NSERVO; i++) plan(i, pos[i], ms, m, now);
    s_drive_until = 0;
    s_torque = true;                           // any move re-energizes the servos
    unlock();
}

void arm_move(const uint16_t pos[NSERVO], uint16_t ms)     { move_all(pos, ms, J_EASE); }
void arm_move_lin(const uint16_t pos[NSERVO], uint16_t ms) { move_all(pos, ms, J_LIN); }

void arm_move_one(int id, uint16_t pos, uint16_t ms)
{
    if (id < 1 || id > NSERVO) return;
    lock();
    plan(id - 1, pos, ms, J_EASE, esp_timer_get_time());
    unlock();
}

void arm_follow_one(int id, uint16_t pos)
{
    s_manual_us = esp_timer_get_time();       // a person is driving: no idle life for a while
    if (id < 1 || id > NSERVO) return;
    lock();
    joint_t *j = &s_j[id - 1];
    j->to = limf(id - 1, pos);
    if (j->m != J_FOLLOW) {
        j->m = J_FOLLOW;                       // keep the current velocity: retargeting mid-drag stays smooth
        j->brake = false;
    }
    unlock();
}

void arm_drive(const float vel[NSERVO], int keep_ms)
{
    s_manual_us = esp_timer_get_time();       // a person is driving: no idle life for a while
    lock();
    bool any = false;
    for (int i = 0; i < NSERVO; i++) {
        joint_t *j = &s_j[i];
        if (vel[i] != 0 || j->m == J_VEL) { j->m = J_VEL; j->vdes = vel[i]; j->brake = false; }
        any |= vel[i] != 0;
    }
    s_drive_until = any ? esp_timer_get_time() + (int64_t)keep_ms * 1000 : 0;
    s_torque = true;
    unlock();
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
        out[i] = bad ? (uint16_t)lroundf(s_j[i].cur) : s_rd[i];
        if (bad) ESP_LOGD(TAG, "servo %d read failed, keeping %d", i + 1, out[i]);
    }
    xSemaphoreGive(s_txn);
    return ok;
}

void arm_torque(bool on)
{
    s_manual_us = esp_timer_get_time();       // a person is driving: no idle life for a while
    if (on) { arm_hold(); return; }
    uint8_t f[16];
    lock();
    s_torque = false;
    for (int i = 0; i < NSERVO; i++) { s_j[i].m = J_HOLD; s_j[i].vel = 0; }
    tx(f, xf_ids(f, XCMD_UNLOAD, IDS, NSERVO));
    unlock();
}

void arm_hold(void)
{
    if (!s_torque) {                           // limp: power up exactly where the hand left it
        uint16_t p[NSERVO];
        bool ok = arm_read(p);
        lock();
        if (ok) place(p);
        else for (int i = 0; i < NSERVO; i++) { s_j[i].m = J_HOLD; s_j[i].sent = -1; }
        s_torque = true;
        unlock();
        return;
    }
    // powered: brake every moving joint to rest where it is now (not where it was headed)
    lock();
    for (int i = 0; i < NSERVO; i++) {
        joint_t *j = &s_j[i];
        if (j->m == J_HOLD) continue;
        j->m = J_VEL; j->vdes = 0; j->brake = true;
    }
    s_drive_until = 0;
    unlock();
}

bool arm_torque_on(void) { return s_torque; }
int64_t arm_manual_us(void) { return s_manual_us; }
void arm_manual_mark(void) { s_manual_us = esp_timer_get_time(); }

void arm_last(uint16_t out[NSERVO])
{
    lock();
    for (int i = 0; i < NSERVO; i++) {
        const joint_t *j = &s_j[i];
        float v = (j->m == J_EASE || j->m == J_LIN || j->m == J_FOLLOW) ? j->to : j->cur;
        out[i] = (uint16_t)lroundf(v);
    }
    unlock();
}

void arm_now(uint16_t out[NSERVO])
{
    lock();
    for (int i = 0; i < NSERVO; i++) out[i] = (uint16_t)lroundf(s_j[i].cur);
    unlock();
}

int  arm_battery_mv(void) { return s_batt; }
bool arm_link_ok(void) { return s_last_rx_us && esp_timer_get_time() - s_last_rx_us < 5000000; }
