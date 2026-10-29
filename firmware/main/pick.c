#include "pick.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "arm.h"
#include "cam.h"
#include "store.h"
#include "fx.h"
#include "moves.h"
#include "kids.h"
#include "chat.h"
#include "esp_log.h"
#include "esp_timer.h"
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

// exact wait, no settle margin (for chained path segments)
static bool wait_raw(int ms)
{
    for (int t = 0; t < ms; t += 10) {
        if (s_abort) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return !s_abort;
}

static bool go(const uint16_t p[NSERVO], int ms) { arm_move(p, ms); return wait_ms(ms); }

// After closing, ease the command back to just past where the jaws actually stopped. Commanding the full
// close against an object (or the jaws against each other) stalls the servo, and a stalled servo hunts:
// that's the buzz/shake while holding. GRIP_PRELOAD keeps enough squeeze to hold.
#define GRIP_PRELOAD 25
static uint16_t s_grip_held;                  // gripper command while holding (0 = full close)
static uint16_t held(void) { return s_grip_held ? s_grip_held : g_cfg.grip_close; }

static void grip_settle(void)
{
    s_grip_held = 0;
    uint16_t now[NSERVO];
    if (!arm_read(now)) return;
    int dir = g_cfg.grip_close > g_cfg.grip_open ? 1 : -1;
    int want = now[0] + dir * GRIP_PRELOAD;
    if ((g_cfg.grip_close - want) * dir > 0) {                    // only ever back off
        s_grip_held = clamp_pos(want);
        arm_move_one(1, s_grip_held, 120);
    }
}

static bool grip(uint16_t v)
{
    arm_move_one(1, v, g_cfg.grip_ms);
    if (!wait_ms(g_cfg.grip_ms)) return false;
    if (v == g_cfg.grip_close) grip_settle();
    return true;
}

static float s_bw, s_bh;                      // size of the last located blob (px)

bool pick_locate(int slot, float *px, float *py)
{
    float sx = 0, sy = 0, sw = 0, sh = 0; int hits = 0;
    for (int i = 0; i < 6; i++) {             // 6 samples over ~0.6 s, need 4 hits
        box_t b;
        if (cam_get(slot, &b)) { sx += b.x; sy += b.y; sw += b.w; sh += b.h; hits++; }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (hits < 4) return false;
    *px = sx / hits; *py = sy / hits;
    s_bw = sw / hits; s_bh = sh / hits;
    return true;
}

// pose = base pose with the gripper set; lift_k scales the approach offset (0 = none)
static void pose(uint16_t out[NSERVO], const uint16_t base[NSERVO], float lift_k, uint16_t grip_v)
{
    for (int i = 0; i < NSERVO; i++) out[i] = clamp_pos(base[i] + (int)(lift_k * g_cfg.lift[i]));
    out[0] = grip_v;
}

static uint16_t hand(void) { return s_holding ? held() : g_cfg.grip_open; }

// head cam: go to the look pose (base shifted by off) and let the camera settle
static bool look_at(int16_t off)
{
    uint16_t p[NSERVO];
    pose(p, g_cfg.look, 0, hand());
    p[5] = clamp_pos(p[5] + off);
    if (!go(p, g_cfg.move_ms)) return false;
    return wait_ms(350);
}

// where is it? fixed cam: just look. head cam: sweep the look pose across the base offsets.
// *base_off = how far the base was turned when it was seen (added to the grasp pose)
static bool find(int slot, float *px, float *py, int *base_off)
{
    *base_off = 0;
    if (!g_cfg.cam_on_head) return pick_locate(slot, px, py);
    for (int i = 0; i < 3; i++) {
        if (i && !g_cfg.scan[i]) continue;
        if (!look_at(g_cfg.scan[i])) return false;
        if (pick_locate(slot, px, py)) { *base_off = g_cfg.scan[i]; return true; }
    }
    return false;
}

bool pick_look(void) { return look_at(0); }

// did the jaws stop on something? reads the real gripper position
static bool holding_check(void)
{
    if (!g_pick2.grab_check) return true;
    uint16_t now[NSERVO];
    if (!arm_read(now)) return true;           // can't tell — assume it's fine
    return !grip_missed(now[0], g_cfg.grip_close, g_cfg.grip_open, g_pick2.grab_margin);
}

// find + grab one object of a color; ends hovering above where it was. false = not found / missed / stopped
static bool grab(int slot)
{
    const char *name = g_cfg.slot_name[slot];
    if (g_cfg.n_calib < 3) { fx_say(SND_NEED_CALIB); set("need_calib", "teach at least 3 calibration points"); return false; }
    int tries = g_pick2.grab_check ? (g_pick2.grab_tries ? g_pick2.grab_tries : 1) : 1;

    for (int t = 1; t <= tries; t++) {
        if (t == 1) fx_say_color(SND_SEARCH_C_RED, SND_SEARCH, name);
        set("searching", t == 1 ? "looking for %s" : "looking for %s again (try %d)", name, t);
        float px, py;
        int off;
        if (!find(slot, &px, &py, &off)) {
            if (!s_abort) { fx_say_color(SND_NOT_FOUND_C_RED, SND_NOT_FOUND, name); set("not_found", "can't see %s", name); }
            return false;
        }
        fx_maybe(SND_FOUND, 70);

        // calibration was taught from the look pose; turning the base just rotates the whole picture
        uint16_t grasp[NSERVO], hover[NSERVO];
        interp_pose(g_cfg.calib, g_cfg.n_calib, px, py, grasp);
        grasp[5] = clamp_pos(grasp[5] + off);
        // long object? turn the wrist so the jaws close across its narrow side
        int twist = align_twist((int)s_bw, (int)s_bh, g_pick2.align_axis, g_pick2.align_turn);
        grasp[1] = clamp_pos(grasp[1] + twist);
        for (int i = 0; i < NSERVO; i++) hover[i] = clamp_pos(grasp[i] + g_cfg.lift[i]);
        grasp[0] = hover[0] = g_cfg.grip_open;

        set("moving", "%s at (%.0f,%.0f)%s", name, px, py, twist ? ", wrist turned" : "");
        if (!go(hover, g_cfg.move_ms)) return false;
        if (!go(grasp, g_cfg.move_ms / 2)) return false;
        fx_maybe(SND_GRAB, 35);
        set("grasping", "%s", name);
        if (!grip(g_cfg.grip_close)) return false;
        if (holding_check()) {
            s_holding = true;
            fx_say(SND_GOT);
            hover[0] = held();
            return go(hover, g_cfg.move_ms / 2);
        }
        // missed: open, back off, look again
        set("grasping", "missed %s — retrying", name);
        fx_say(SND_MISSED);
        if (!grip(g_cfg.grip_open)) return false;
        if (!go(hover, g_cfg.move_ms / 2)) return false;
    }
    fx_say(SND_ERROR);
    set("not_found", "couldn't get a grip on %s", name);
    return false;
}

// carry to a pose, let go, back off upward
static bool release_at(const uint16_t target[NSERVO])
{
    uint16_t p[NSERVO];
    pose(p, target, 1.0f, held());
    if (!go(p, g_cfg.move_ms)) return false;
    pose(p, target, 0.0f, held());
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
    fx_say(SND_DROP);
    set("dropping", "to drop pose");
    uint16_t p[NSERVO];
    pose(p, g_cfg.drop, 0, held());
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
    fx_say(SND_GIVE);
    set("giving", "here you go");
    uint16_t p[NSERVO];
    pose(p, g_cfg.give, 0, held());
    if (!go(p, g_cfg.move_ms)) return;
    if (!wait_ms(700)) return;
    if (!grip(g_cfg.grip_open)) return;
    s_holding = false;
    fx_say(SND_GIVEN);
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
        for (int k = 0; k < (g_cfg.cam_on_head ? 3 : 1) && slot < 0; k++) {
            if (g_cfg.cam_on_head && (k == 0 || g_cfg.scan[k]) && !look_at(g_cfg.scan[k])) return;
            for (int s = 0; s < NSLOT && slot < 0; s++) { float x, y; if (pick_locate(s, &x, &y)) slot = s; }
        }
        if (slot < 0) break;
        set("sorting", "%s → bin %d (%d done)", g_cfg.slot_name[slot], slot + 1, done);
        if (!grab(slot)) { if (s_abort) return; break; }
        set("sorting", "%s → bin %d", g_cfg.slot_name[slot], slot + 1);
        if (!release_at(g_cfg.bin[slot])) return;
        done++;
    }
    if (!go_home()) return;
    fx_say(done ? SND_SORTED : SND_NOTHING);
    set(done ? "idle" : "not_found", done ? "sorted %d" : "nothing to sort", done);
}

// pick every visible object (either color, biggest first), small ones to bin 1, big ones to bin 2
static void do_sort_size(void)
{
    if (s_holding) { set("holding", "drop what I'm holding first"); return; }
    int done = 0;
    for (int round = 0; round < 12; round++) {
        int slot = -1;
        float bestA = 0;
        for (int k = 0; k < (g_cfg.cam_on_head ? 3 : 1); k++) {
            if (g_cfg.cam_on_head && (k == 0 || g_cfg.scan[k]) && !look_at(g_cfg.scan[k])) return;
            for (int s = 0; s < NSLOT; s++) {
                float x, y;
                if (pick_locate(s, &x, &y) && s_bw * s_bh > bestA) { bestA = s_bw * s_bh; slot = s; }
            }
            if (slot >= 0) break;
        }
        if (slot < 0) break;
        if (!grab(slot)) { if (s_abort) return; break; }
        int bin = size_bin((int)s_bw, (int)s_bh, g_pick2.size_split);
        set("sorting", "%s → %s bin (%d done)", g_cfg.slot_name[slot], bin ? "big" : "small", done);
        if (!release_at(g_cfg.bin[bin])) return;
        done++;
    }
    if (!go_home()) return;
    fx_say(done ? SND_SORTED : SND_NOTHING);
    set(done ? "idle" : "not_found", done ? "sorted %d by size" : "nothing to sort", done);
}

// ---- tricks: small, safe moves around Home (base + wrist rotate + gripper, no big gravity loads)
static bool beat(uint16_t p[NSERVO], int ms) { return go(p, ms); }

static void do_wave(void)
{
    fx_say(SND_WAVE);
    set("waving", "hello!");
    uint16_t p[NSERVO];
    pose(p, g_cfg.home, 0.6f, g_cfg.grip_open);              // a bit raised
    if (!go(p, g_cfg.move_ms)) return;
    for (int i = 0; i < 4; i++) {
        p[1] = clamp_pos(g_cfg.home[1] + (i % 2 ? -170 : 170));  // wrist rotate
        p[0] = i % 2 ? g_cfg.grip_open : g_cfg.grip_close;        // hand opens and closes
        if (!beat(p, 380)) return;          // 340-unit swing: faster than ~380 ms outruns the servo
    }
    if (!go_home()) return;
    set("idle", "waved");
}

static void do_bow(void)
{
    fx_say(SND_BOW);
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
    fx_say(SND_DANCE);
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
    fx_say(SND_BELL);
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

// ---- third hand: present the open gripper at the helper spot; then grip / nudge / let go by voice
static void do_helper(void)
{
    fx_say(SND_HELPER);
    set("helping", "put it in my hand, then say \"hold it\"");
    uint16_t p[NSERVO];
    pose(p, g_cfg.helper, 0, hand());
    if (!go(p, g_cfg.move_ms)) return;
    if (!s_holding && !grip(g_cfg.grip_open)) return;
    set("helping", s_holding ? "holding it — nudge me, or say \"let go\"" : "ready — say \"hold it\"");
}

// small step from wherever the arm is now. joint space: base, lift vector, wrist rotate, wrist tilt
static void do_nudge(int dir)
{
    uint16_t p[NSERVO];
    arm_last(p);
    switch (dir) {
    case NUDGE_LEFT:      p[5] = clamp_pos(p[5] + 25); break;
    case NUDGE_RIGHT:     p[5] = clamp_pos(p[5] - 25); break;
    case NUDGE_UP:        for (int i = 1; i < NSERVO; i++) p[i] = clamp_pos(p[i] + g_cfg.lift[i] / 4); break;
    case NUDGE_DOWN:      for (int i = 1; i < NSERVO; i++) p[i] = clamp_pos(p[i] - g_cfg.lift[i] / 4); break;
    case NUDGE_TURN_L:    p[1] = clamp_pos(p[1] + 40); break;
    case NUDGE_TURN_R:    p[1] = clamp_pos(p[1] - 40); break;
    case NUDGE_TILT_UP:   p[2] = clamp_pos(p[2] + 30); break;
    case NUDGE_TILT_DOWN: p[2] = clamp_pos(p[2] - 30); break;
    }
    go(p, 180);
}

// ---- stacking challenge: keep grabbing blocks (center look only — keep the tower off to the side,
// out of that view) and set each one a level higher on the tower spot. Score = blocks placed.
static void do_stack(void)
{
    if (s_holding) { set("holding", "drop what I'm holding first"); return; }
    if (g_cfg.n_calib < 3) { fx_say(SND_NEED_CALIB); set("need_calib", "teach at least 3 calibration points"); return; }
    int level = 0;
    for (; level < 10; level++) {
        int slot = -1, off = 0;
        float px = 0, py = 0;
        if (g_cfg.cam_on_head && !look_at(g_cfg.scan[0])) return;
        for (int s = 0; s < NSLOT && slot < 0; s++) if (pick_locate(s, &px, &py)) slot = s;
        if (slot < 0) break;
        set("stacking", "%d high — getting the next block", level);
        uint16_t grasp[NSERVO], hover[NSERVO];
        interp_pose(g_cfg.calib, g_cfg.n_calib, px, py, grasp);
        grasp[5] = clamp_pos(grasp[5] + off);
        for (int i = 0; i < NSERVO; i++) hover[i] = clamp_pos(grasp[i] + g_cfg.lift[i]);
        grasp[0] = hover[0] = g_cfg.grip_open;
        if (!go(hover, g_cfg.move_ms) || !go(grasp, g_cfg.move_ms / 2)) return;
        set("grasping", "block %d", level + 1);
        if (!grip(g_cfg.grip_close)) return;
        s_holding = true;
        hover[0] = held();
        if (!go(hover, g_cfg.move_ms / 2)) return;
        // place: tower pose raised by level * block height, slow and gentle on the way down
        set("stacking", "placing block %d", level + 1);
        float k = level * g_cfg.stack_pct / 100.0f;
        uint16_t above[NSERVO], place[NSERVO];
        pose(above, g_cfg.tower, k + 1.0f, held());
        pose(place, g_cfg.tower, k, held());
        if (!go(above, g_cfg.move_ms) || !go(place, g_cfg.move_ms)) return;
        if (!grip(g_cfg.grip_open)) return;
        s_holding = false;
        place[0] = above[0] = g_cfg.grip_open;
        fx_say_n(SND_STACK_N_1, level + 1);
        if (!go(above, g_cfg.move_ms / 2)) return;
    }
    if (!go_home()) return;
    fx_say(level ? SND_TOWER : SND_NOTHING);
    kids_score("tower", level);                // a new best also cheers (kids.c)
    set(level ? "idle" : "not_found", level ? "tower of %d!" : "no blocks to stack", level);
}

// ---- teaching: Muffin explains a bit of robotics, sentence by sentence, moving the part it's talking about.
// The words come from LESSONS in tools/make_sounds.py; the moves are matched here by lesson + sentence.
static bool wait_quiet(void)                  // until the current sentence has finished playing
{
    for (int t = 0; t < 30000; t += 40) {
        if (s_abort) return false;
        if (!fx_speaking()) return wait_raw(220);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    return !s_abort;
}

// one joint out and back around Home, eased
static bool wiggle(int i, int amt)
{
    uint16_t p[NSERVO];
    pose(p, g_cfg.home, 0, hand());
    p[i] = clamp_pos(g_cfg.home[i] + amt);
    if (!go(p, 600)) return false;
    p[i] = clamp_pos(g_cfg.home[i] - amt);
    if (!go(p, 900)) return false;
    p[i] = g_cfg.home[i];
    return go(p, 600);
}

static bool teach_move(int lesson, int k)
{
    uint16_t p[NSERVO];
    switch (lesson) {
    case LESSON_ROBOT:
        if (k == 2 && g_cfg.cam_on_head) return look_at(0);                       // "I sense with my camera"
        if (k == 4) {                                                              // "I act... like this"
            if (!go_home()) return false;
            return wiggle(1, 150);
        }
        if (k == 5) return go_home();
        break;
    case LESSON_SERVO:
        if (k == 2) return wiggle(5, 150);                                         // the base finds its spot
        break;
    case LESSON_JOINTS: {                                                          // "One. My base spins." ...
        static const int8_t idx[7] = {-1, 5, 4, 3, 2, 1, 0};
        static const int16_t amt[7] = {0, 120, 60, 60, 90, 160, 0};
        if (k >= 1 && k <= 5) return wiggle(idx[k], amt[k]);
        if (k == 6) return grip(g_cfg.grip_open) && grip(g_cfg.grip_close) && grip(g_cfg.grip_open);
        break;
    }
    case LESSON_GRIPPER:
        if (k == 0) return grip(g_cfg.grip_open);
        if (k == 1) return grip(g_cfg.grip_close);                                // closes, then eases off (grip_settle)
        if (k == 3) return grip(g_cfg.grip_open);
        break;
    case LESSON_VISION:
        if (k == 0) return look_at(0);
        if (k == 4) return go_home();
        break;
    case LESSON_SMOOTH:
        pose(p, g_cfg.home, 0, hand());
        if (k == 0) {                                                              // the bad way: constant speed, hard stops
            p[5] = clamp_pos(g_cfg.home[5] + 150); arm_move_lin(p, 260); if (!wait_raw(420)) return false;
            p[5] = clamp_pos(g_cfg.home[5] - 150); arm_move_lin(p, 380); if (!wait_raw(540)) return false;
            p[5] = g_cfg.home[5];                  arm_move_lin(p, 260); return wait_raw(420);
        }
        if (k == 2) {                                                              // the good way: eased
            p[5] = clamp_pos(g_cfg.home[5] + 150); if (!go(p, 900)) return false;
            p[5] = clamp_pos(g_cfg.home[5] - 150); if (!go(p, 1300)) return false;
            p[5] = g_cfg.home[5];                  return go(p, 900);
        }
        break;
    case LESSON_REACH:
        if (k == 0 && g_cfg.n_calib > 0) {                                        // reach over a taught spot
            pose(p, g_cfg.calib[0].pos, 1.0f, hand());
            return go(p, g_cfg.move_ms);
        }
        if (k == 3) return go_home();
        break;
    }
    return true;
}

static void do_teach(int lesson)
{
    if (lesson < 0 || lesson >= LESSON_COUNT) lesson = 0;
    set("teaching", "lesson %d of %d", lesson + 1, LESSON_COUNT);
    if (!go_home()) return;
    for (int k = 0; k < LESSON[lesson].n; k++) {
        fx_say(LESSON[lesson].first + k);
        if (!teach_move(lesson, k) || !wait_quiet()) return;
    }
    if (!go_home()) return;
    set(s_holding ? "holding" : "idle", "lesson done");
    chat_lesson_done(lesson);
}

// replay a recorded move: glide to its start, then follow a smooth curve through the frames in real
// time (Catmull-Rom, 4 constant-speed sub-steps per recorded frame — no stops between frames)
static void do_play(int idx)
{
    static uint16_t f[MOVE_FRAMES][NSERVO];
    int n = moves_load(idx, f, MOVE_FRAMES);
    if (n < 2) { set("idle", "that move is empty"); return; }
    fx_say(SND_PLAY);
    set("playing", "%s", moves_name(idx));
    if (!go(f[0], g_cfg.move_ms)) return;
    const int sub = 4, seg_ms = 1000 / MOVE_HZ / sub;
    for (int i = 0; i < n - 1; i++) {
        const uint16_t *a = f[i ? i - 1 : 0], *b = f[i], *c = f[i + 1], *d = f[i + 2 < n ? i + 2 : n - 1];
        for (int k = 1; k <= sub; k++) {
            uint16_t p[NSERVO];
            float t = (float)k / sub;
            for (int j = 0; j < NSERVO; j++) p[j] = clamp_pos((int)lroundf(catmull(a[j], b[j], c[j], d[j], t)));
            arm_move_lin(p, seg_ms);
            if (!wait_raw(seg_ms)) return;
        }
    }
    if (!wait_ms(0) || !go_home()) return;
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
        {   // asked for the same show twice in a row? it has opinions about that
            static int last_op = -1, last_slot = -1;
            static int64_t last_t;
            int64_t now = esp_timer_get_time();
            bool show = j.op == OP_WAVE || j.op == OP_BOW || j.op == OP_DANCE || j.op == OP_BELL || j.op == OP_PLAY ||
                        j.op == OP_STACK || j.op == OP_SORT || j.op == OP_SORT_SIZE || j.op == OP_PICK;
            if (show && (int)j.op == last_op && j.slot == last_slot && now - last_t < 90000000) fx_maybe(SND_AGAIN, 60);
            last_op = j.op; last_slot = j.slot; last_t = now;
        }
        switch (j.op) {
        case OP_PICK:  do_pick(j.slot); break;
        case OP_DROP:  do_drop(); break;
        case OP_GIVE:  do_give(); break;
        case OP_SORT:  do_sort(); break;
        case OP_SORT_SIZE: do_sort_size(); break;
        case OP_WAVE:  do_wave(); break;
        case OP_BOW:   do_bow(); break;
        case OP_DANCE: do_dance(); break;
        case OP_BELL:  do_bell(); break;
        case OP_PLAY:  do_play(j.slot); break;
        case OP_HELPER: do_helper(); break;
        case OP_STACK: do_stack(); break;
        case OP_NUDGE: do_nudge(j.slot); break;
        case OP_TEACH: do_teach(j.slot); break;
        case OP_HOME: {
            uint16_t p[NSERVO]; memcpy(p, g_cfg.home, sizeof p);
            p[0] = s_holding ? held() : g_cfg.grip_open;
            fx_maybe(SND_HOME, 50);
            set("moving", "home");
            if (go(p, g_cfg.move_ms)) set(s_holding ? "holding" : "idle", "home");
            break;
        }
        case OP_OPEN:  fx_say(strcmp(s_state, "helping") ? SND_OK : SND_RELEASED); set("moving", "open"); if (grip(g_cfg.grip_open)) { s_holding = false; set("idle", "hand open"); } break;
        case OP_CLOSE: fx_say(strcmp(s_state, "helping") ? SND_OK : SND_HELP_HOLD); set("moving", "close"); if (grip(g_cfg.grip_close)) { s_holding = true; set("holding", "hand closed"); } break;
        }
        if (s_abort) set("stopped", "stopped");
        s_busy = false;
        bool show = j.op == OP_WAVE || j.op == OP_BOW || j.op == OP_DANCE || j.op == OP_BELL || j.op == OP_PLAY || j.op == OP_STACK;
        if (show && !s_abort) chat_show_done(j.op, j.slot);
    }
}

void pick_init(void)
{
    s_q = xQueueCreate(1, sizeof(job_t));
    xTaskCreate(task, "pick", 4096, NULL, 5, NULL);
}

bool pick_run(op_t op, int slot, char *msg, size_t n)
{
    if (s_busy || uxQueueMessagesWaiting(s_q)) {
        static int64_t said;
        int64_t now = esp_timer_get_time();
        if (now - said > 4000000) { said = now; fx_say(SND_BUSY); }
        snprintf(msg, n, "busy (%s) — say stop", s_state);
        return false;
    }
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
