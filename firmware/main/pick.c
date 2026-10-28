#include "pick.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "arm.h"
#include "cam.h"
#include "store.h"
#include "fx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "pick";
typedef struct { op_t op; int slot; } job_t;

static QueueHandle_t s_q;
static volatile bool s_busy, s_abort, s_holding;
static char s_state[16] = "idle", s_detail[64] = "";

static void set(const char *st, const char *fmt, ...)
{
    bool changed = strcmp(s_state, st) != 0;
    strlcpy(s_state, st, sizeof s_state);
    if (changed) fx_state(st);
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_detail, sizeof s_detail, fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s: %s", s_state, s_detail);
}

// wait for a move to finish, bail early on stop
static bool wait_ms(int ms)
{
    for (int t = 0; t < ms + 120; t += 20) {
        if (s_abort) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return !s_abort;
}

static bool go(const uint16_t p[NSERVO], int ms) { arm_move(p, ms); return wait_ms(ms); }

static bool grip(uint16_t v)
{
    arm_move_one(1, v, g_cfg.grip_ms);
    return wait_ms(g_cfg.grip_ms);
}

bool pick_locate(int slot, float *px, float *py)
{
    float sx = 0, sy = 0; int hits = 0;
    for (int i = 0; i < 6; i++) {             // 6 samples over ~0.6 s, need 4 hits
        box_t b;
        if (cam_get(slot, &b)) { sx += b.x; sy += b.y; hits++; }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (hits < 4) return false;
    *px = sx / hits; *py = sy / hits;
    return true;
}

static void do_pick(int slot)
{
    const char *name = g_cfg.slot_name[slot];
    if (s_holding) { set("holding", "already holding something — drop it first"); return; }
    if (g_cfg.n_calib < 3) { set("need_calib", "teach at least 3 calibration points"); return; }

    set("searching", "looking for %s", name);
    float px, py;
    if (!pick_locate(slot, &px, &py)) { set("not_found", "can't see %s", name); return; }

    uint16_t grasp[NSERVO], hover[NSERVO];
    interp_pose(g_cfg.calib, g_cfg.n_calib, px, py, grasp);
    for (int i = 0; i < NSERVO; i++) hover[i] = clamp_pos(grasp[i] + g_cfg.lift[i]);
    grasp[0] = hover[0] = g_cfg.grip_open;

    set("moving", "%s at (%.0f,%.0f)", name, px, py);
    if (!go(hover, g_cfg.move_ms)) return;
    if (!go(grasp, g_cfg.move_ms / 2)) return;
    set("grasping", "%s", name);
    if (!grip(g_cfg.grip_close)) return;
    hover[0] = g_cfg.grip_close;
    if (!go(hover, g_cfg.move_ms / 2)) return;
    uint16_t carry[NSERVO];
    memcpy(carry, g_cfg.home, sizeof carry);
    carry[0] = g_cfg.grip_close;
    if (!go(carry, g_cfg.move_ms)) return;
    s_holding = true;
    set("holding", "%s", name);
}

static void do_drop(void)
{
    uint16_t p[NSERVO];
    memcpy(p, g_cfg.drop, sizeof p);
    p[0] = g_cfg.grip_close;
    set("dropping", "to drop pose");
    if (!go(p, g_cfg.move_ms)) return;
    if (!grip(g_cfg.grip_open)) return;
    s_holding = false;
    memcpy(p, g_cfg.home, sizeof p);
    p[0] = g_cfg.grip_open;
    if (!go(p, g_cfg.move_ms)) return;
    set("idle", "dropped");
}

static void task(void *arg)
{
    job_t j;
    for (;;) {
        xQueueReceive(s_q, &j, portMAX_DELAY);
        s_abort = false;
        s_busy = true;
        switch (j.op) {
        case OP_PICK:  do_pick(j.slot); break;
        case OP_DROP:  do_drop(); break;
        case OP_HOME: {
            uint16_t p[NSERVO]; memcpy(p, g_cfg.home, sizeof p);
            p[0] = s_holding ? g_cfg.grip_close : g_cfg.grip_open;
            set("moving", "home");
            if (go(p, g_cfg.move_ms)) set(s_holding ? "holding" : "idle", "home");
            break;
        }
        case OP_OPEN:  set("moving", "open"); if (grip(g_cfg.grip_open)) { s_holding = false; set("idle", "hand open"); } break;
        case OP_CLOSE: set("moving", "close"); if (grip(g_cfg.grip_close)) set("idle", "hand closed"); break;
        }
        if (s_abort) set("stopped", "stopped");
        s_busy = false;
    }
}

void pick_init(void)
{
    s_q = xQueueCreate(1, sizeof(job_t));
    xTaskCreate(task, "pick", 4096, NULL, 5, NULL);
}

bool pick_run(op_t op, int slot, char *msg, size_t n)
{
    if (s_busy || uxQueueMessagesWaiting(s_q)) { snprintf(msg, n, "busy (%s) — say stop", s_state); return false; }
    job_t j = {op, slot};
    xQueueSend(s_q, &j, 0);
    snprintf(msg, n, "ok");
    return true;
}

void pick_stop(void)
{
    s_abort = true;
    xQueueReset(s_q);
    arm_hold();
}

bool pick_busy(void) { return s_busy; }
bool pick_holding(void) { return s_holding; }
void pick_status(char *st, size_t ns, char *d, size_t nd)
{
    strlcpy(st, s_state, ns);
    strlcpy(d, s_detail, nd);
}
