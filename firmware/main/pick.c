#include "pick.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "arm.h"
#include "cam.h"
#include "store.h"
#include "fx.h"
#include "moves.h"
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

// pose = base pose with the gripper set; lift_k scales the approach offset (0 = none)
static void pose(uint16_t out[NSERVO], const uint16_t base[NSERVO], float lift_k, uint16_t grip_v)
{
    for (int i = 0; i < NSERVO; i++) out[i] = clamp_pos(base[i] + (int)(lift_k * g_cfg.lift[i]));
    out[0] = grip_v;
}

static uint16_t hand(void) { return s_holding ? g_cfg.grip_close : g_cfg.grip_open; }

// find + grab one object of a color; ends hovering above where it was. false = not found / stopped
static bool grab(int slot)
{
    const char *name = g_cfg.slot_name[slot];
    if (g_cfg.n_calib < 3) { set("need_calib", "teach at least 3 calibration points"); return false; }

    set("searching", "looking for %s", name);
    float px, py;
    if (!pick_locate(slot, &px, &py)) { set("not_found", "can't see %s", name); return false; }

    uint16_t grasp[NSERVO], hover[NSERVO];
    interp_pose(g_cfg.calib, g_cfg.n_calib, px, py, grasp);
    for (int i = 0; i < NSERVO; i++) hover[i] = clamp_pos(grasp[i] + g_cfg.lift[i]);
    grasp[0] = hover[0] = g_cfg.grip_open;

    set("moving", "%s at (%.0f,%.0f)", name, px, py);
    if (!go(hover, g_cfg.move_ms)) return false;
    if (!go(grasp, g_cfg.move_ms / 2)) return false;
    set("grasping", "%s", name);
    if (!grip(g_cfg.grip_close)) return false;
    s_holding = true;
    hover[0] = g_cfg.grip_close;
    return go(hover, g_cfg.move_ms / 2);
}

// carry to a pose, let go, back off upward
static bool release_at(const uint16_t target[NSERVO])
{
    uint16_t p[NSERVO];
    pose(p, target, 1.0f, g_cfg.grip_close);
    if (!go(p, g_cfg.move_ms)) return false;
    pose(p, target, 0.0f, g_cfg.grip_close);
    if (!go(p, g_cfg.move_ms / 2)) return false;
    if (!grip(g_cfg.grip_open)) return false;
    s_holding = false;
    pose(p, target, 1.0f, g_cfg.grip_open);
    return go(p, g_cfg.move_ms / 2);
}

static bool go_home(void) { uint16_t p[NSERVO]; pose(p, g_cfg.home, 0, hand()); return go(p, g_cfg.move_ms); }

static void do_pick(int slot)
{
    if (s_holding) { set("holding", "already holding something — drop it first"); return; }
    if (!grab(slot)) return;
    if (!go_home()) return;
    set("holding", "%s", g_cfg.slot_name[slot]);
}

static void do_drop(void)
{
    set("dropping", "to drop pose");
    uint16_t p[NSERVO];
    pose(p, g_cfg.drop, 0, g_cfg.grip_close);
    if (!go(p, g_cfg.move_ms)) return;
    if (!grip(g_cfg.grip_open)) return;
    s_holding = false;
    if (!go_home()) return;
    set("idle", "dropped");
}

// bring it to the hand-off spot, wait a beat for a hand, let go
static void do_give(void)
{
    if (!s_holding) { set("idle", "nothing to give — grab something first"); return; }
    set("giving", "here you go");
    uint16_t p[NSERVO];
    pose(p, g_cfg.give, 0, g_cfg.grip_close);
    if (!go(p, g_cfg.move_ms)) return;
    if (!wait_ms(700)) return;
    if (!grip(g_cfg.grip_open)) return;
    s_holding = false;
    if (!wait_ms(500) || !go_home()) return;
    set("idle", "handed over");
}

// pick every visible object, each color into its own bin, until the mat is clear
static void do_sort(void)
{
    if (s_holding) { set("holding", "drop what I'm holding first"); return; }
    int done = 0;
    for (int round = 0; round < 12; round++) {
        int slot = -1;
        for (int s = 0; s < NSLOT && slot < 0; s++) { box_t b; if (cam_get(s, &b)) slot = s; }
        if (slot < 0) break;
        set("sorting", "%s → bin %d (%d done)", g_cfg.slot_name[slot], slot + 1, done);
        if (!grab(slot)) { if (s_abort) return; break; }
        set("sorting", "%s → bin %d", g_cfg.slot_name[slot], slot + 1);
        if (!release_at(g_cfg.bin[slot])) return;
        done++;
    }
    if (!go_home()) return;
    set(done ? "idle" : "not_found", done ? "sorted %d" : "nothing to sort", done);
}

// ---- tricks: small, safe moves around Home (base + wrist rotate + gripper, no big gravity loads)
static bool beat(uint16_t p[NSERVO], int ms) { return go(p, ms); }

static void do_wave(void)
{
    set("waving", "hello!");
    uint16_t p[NSERVO];
    pose(p, g_cfg.home, 0.6f, g_cfg.grip_open);              // a bit raised
    if (!go(p, g_cfg.move_ms)) return;
    for (int i = 0; i < 4; i++) {
        p[1] = clamp_pos(g_cfg.home[1] + (i % 2 ? -170 : 170));  // wrist rotate
        p[0] = i % 2 ? g_cfg.grip_open : g_cfg.grip_close;        // hand opens and closes
        if (!beat(p, 260)) return;
    }
    if (!go_home()) return;
    set("idle", "waved");
}

static void do_bow(void)
{
    set("bowing", "thank you!");
    uint16_t p[NSERVO];
    pose(p, g_cfg.home, -0.8f, hand());                      // opposite of lift = lean forward/down
    if (!go(p, 900)) return;
    if (!wait_ms(500)) return;
    if (!go_home()) return;
    set(s_holding ? "holding" : "idle", "bowed");
}

static void do_dance(void)
{
    set("dancing", "♪");
    uint16_t p[NSERVO];
    static const int8_t base[8] = {1, -1, 1, -1, 2, -2, 0, 0}, wrist[8] = {1, 1, -1, -1, 1, -1, 1, 0};
    for (int i = 0; i < 8; i++) {
        pose(p, g_cfg.home, (i % 2) ? 0.5f : 0.2f, (i % 2) ? g_cfg.grip_close : g_cfg.grip_open);
        p[5] = clamp_pos(g_cfg.home[5] + 90 * base[i]);
        p[1] = clamp_pos(g_cfg.home[1] + 140 * wrist[i]);
        if (!beat(p, 330)) return;
    }
    if (!go_home()) return;
    set("idle", "danced");
}

// hover over the bell, strike 3 times
static void do_bell(void)
{
    set("ringing", "ding!");
    uint16_t up[NSERVO], hit[NSERVO];
    pose(up, g_cfg.bell, 0.5f, g_cfg.grip_close);
    pose(hit, g_cfg.bell, 0.0f, g_cfg.grip_close);
    if (!go(up, g_cfg.move_ms)) return;
    for (int i = 0; i < 3; i++) {
        if (!go(hit, 110)) return;
        fx_event(FX_BELL);
        if (!go(up, 220)) return;
        if (!wait_ms(250)) return;
    }
    if (!go_home()) return;
    set("idle", "rang the bell");
}

// replay a recorded move: glide to its start, then stream frames at the record rate
static void do_play(int idx)
{
    static uint16_t f[MOVE_FRAMES][NSERVO];
    int n = moves_load(idx, f, MOVE_FRAMES);
    if (n < 2) { set("idle", "that move is empty"); return; }
    set("playing", "%s", moves_name(idx));
    if (!go(f[0], g_cfg.move_ms)) return;
    for (int i = 1; i < n; i++) if (!go(f[i], 1000 / MOVE_HZ - 120)) return;   // wait_ms adds 120 settle
    if (!go_home()) return;
    s_holding = false;
    set("idle", "played %s", moves_name(idx));
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
        case OP_GIVE:  do_give(); break;
        case OP_SORT:  do_sort(); break;
        case OP_WAVE:  do_wave(); break;
        case OP_BOW:   do_bow(); break;
        case OP_DANCE: do_dance(); break;
        case OP_BELL:  do_bell(); break;
        case OP_PLAY:  do_play(j.slot); break;
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
