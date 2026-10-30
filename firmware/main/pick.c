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
#include "school.h"
#include "voice.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include <stdlib.h>
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

// ---- Moving like it means it. Cues from animation and HRI research (CLAUDE.md "Lifelike motion"):
//   look first      the head (wrist + camera) turns toward where it's going, and the arm follows
//   anticipation    a small wind-up the other way, and a breath in, before a big swing
//   arcs            the hand rises through the middle of a long move instead of sliding straight
//   overlap         big joints lead and small ones trail: base, shoulder, elbow, wrist, twist, gripper
//   follow-through  the base carries a touch past the target and settles back
//   reactions       a quick proud bob on success; a slow slump and a head shake on failure
//   idle life       slow breathing and the odd glance while someone's around; head down while asleep
// Only travel between poses gets this. Fine moves (down onto an object, setting it down) stay plain and exact,
// and every lifelike move ends exactly on its target.
static bool s_anim = true;                    // NVS "anim" (Workshop -> Settings: Lifelike motion)
static const uint16_t LAG[NSERVO] = {210, 180, 150, 100, 50, 0};   // ms each joint trails (index 0 = gripper)

void pick_anim(bool on)
{
    s_anim = on;
    nvs_handle_t h;
    if (nvs_open("xarm", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "anim", on); nvs_commit(h); nvs_close(h); }
}
bool pick_anim_on(void) { return s_anim; }

static int jitter(int ms) { return ms + (int)(esp_random() % (uint32_t)(ms / 5 + 1)) - ms / 10; }   // ±10%: never robotic-identical
static int sgn(int v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// mood (chat.c, -3 grumpy .. 3 delighted) shows in every move: delighted is bigger and brisker, grumpy smaller
// and dragging. People read emotion from size and speed alone.
static float mood_k(void) { return 1.0f + 0.12f * chat_mood(); }                  // 0.64 .. 1.36
static int mood_ms(int ms) { return ms * (100 - 6 * chat_mood()) / 100; }        // 0.82x .. 1.18x

static int big_delta(const uint16_t a[NSERVO], const uint16_t b[NSERVO])
{
    int m = 0;
    for (int i = 3; i < NSERVO; i++) { int d = abs((int)a[i] - (int)b[i]); if (d > m) m = d; }   // elbow, shoulder, base
    return m;
}

// stream an arcing, overlapping move at the arm task's own 25 Hz, then land exactly
static bool stream_arc(const uint16_t from[NSERVO], const uint16_t to[NSERVO], const int16_t bulge[NSERVO], int ms)
{
    const int step = 40;
    uint16_t p[NSERVO];
    for (int t = step; t <= ms + LAG[0]; t += step) {
        arc_pose(from, to, bulge, LAG, (float)ms, (float)t, p);
        arm_move_lin(p, step);
        if (!wait_raw(step)) return false;
    }
    arm_move(to, 120);
    return wait_raw(160);
}

static bool celebrate_moves(void);

static bool travel(const uint16_t target[NSERVO], int ms)
{
    uint16_t from[NSERVO], to[NSERVO];
    arm_last(from);
    memcpy(to, target, sizeof to);
    int big = big_delta(from, to), db = (int)to[5] - (int)from[5];
    if (!s_anim || big < 90) return go(to, ms);
    ms = mood_ms(jitter(ms));
    float mk = mood_k();
    uint16_t p[NSERVO];
    if (big >= 160) {
        // wind-up: a breath in (up, along the taught lift direction) and a small turn away from where it's going
        pose(p, from, 0.12f * mk, from[0]);
        p[5] = clamp_pos(from[5] - sgn(db) * clampi((int)(abs(db) * 6 / 100 * mk), 0, 30));
        if (!go(p, jitter(180))) return false;
        // look first: the head turns and aims before the arm commits, then a beat to "think"
        p[5] = clamp_pos(from[5] + db * 3 / 10);
        p[2] = clamp_pos(from[2] + ((int)to[2] - (int)from[2]) / 2);
        p[1] = clamp_pos(from[1] + ((int)to[1] - (int)from[1]) * 6 / 10);
        if (!go(p, jitter(260)) || !wait_ms(jitter(60))) return false;
        arm_last(from);
    }
    // the arc: rise through the middle, more for longer moves
    float k = big / 400.0f * mk;
    k = k < 0.12f ? 0.12f : k > 0.55f ? 0.55f : k;
    int16_t bulge[NSERVO] = {0};
    for (int i = 2; i <= 4; i++) bulge[i] = (int16_t)(g_cfg.lift[i] * k);
    if (!stream_arc(from, to, bulge, ms)) return false;
    // follow-through: the base carries a touch past, then settles
    if (abs(db) >= 120) {
        memcpy(p, to, sizeof p);
        p[5] = clamp_pos(to[5] + sgn(db) * clampi((int)(abs(db) * 4 / 100 * mk), 5, 20));
        arm_move(p, 110);
        if (!wait_raw(110)) return false;
        return go(to, 260);
    }
    return true;
}

// "hmm": looks up and off to one side, a beat, then back. Forethought before a verdict reads as thinking.
static bool think(void)
{
    if (!s_anim) return true;
    uint16_t b[NSERVO], p[NSERVO];
    arm_last(b);
    pose(p, b, 0.15f, b[0]);
    p[5] = clamp_pos(b[5] + (esp_random() & 1 ? 50 : -50));
    p[1] = clamp_pos(b[1] - 40);
    return go(p, mood_ms(jitter(450))) && wait_ms(jitter(350)) && go(b, mood_ms(jitter(380)));
}

// a quick proud bob, or a slow slump and a little head shake; ends where it started
static bool react(bool good)
{
    if (!s_anim) return true;
    uint16_t b[NSERVO], p[NSERVO];
    arm_last(b);
    if (good) {
        pose(p, b, 0.22f * mood_k(), b[0]);
        return go(p, jitter(170)) && go(b, jitter(300));
    }
    pose(p, b, -0.22f, b[0]);
    if (!go(p, jitter(700))) return false;
    for (int i = 0; i < 2; i++) { p[1] = clamp_pos(b[1] + (i ? -70 : 70)); if (!go(p, jitter(260))) return false; }
    p[1] = b[1];
    return go(p, 200) && go(b, jitter(600));
}

// head cam: go to the look pose (base shifted by off) and let the camera settle
static bool look_at(int16_t off)
{
    uint16_t p[NSERVO];
    pose(p, g_cfg.look, 0, hand());
    p[5] = clamp_pos(p[5] + off);
    if (!travel(p, g_cfg.move_ms)) return false;
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
            if (!s_abort) { fx_say_color(SND_NOT_FOUND_C_RED, SND_NOT_FOUND, name); set("not_found", "can't see %s", name); react(false); }
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
        if (!travel(hover, g_cfg.move_ms)) return false;
        if (!go(grasp, g_cfg.move_ms / 2)) return false;
        fx_maybe(SND_GRAB, 35);
        set("grasping", "%s", name);
        if (!grip(g_cfg.grip_close)) return false;
        if (holding_check()) {
            s_holding = true;
            fx_say(SND_GOT);
            hover[0] = held();
            return go(hover, g_cfg.move_ms / 2) && react(true);
        }
        // missed: open, back off, look again
        set("grasping", "missed %s — retrying", name);
        fx_say(SND_MISSED);
        if (!grip(g_cfg.grip_open)) return false;
        if (!go(hover, g_cfg.move_ms / 2) || !react(false)) return false;
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
    if (!travel(p, g_cfg.move_ms)) return false;
    pose(p, target, 0.0f, held());
    if (!go(p, g_cfg.move_ms / 2)) return false;
    if (!grip(g_cfg.grip_open)) return false;
    s_holding = false;
    pose(p, target, 1.0f, g_cfg.grip_open);
    return go(p, g_cfg.move_ms / 2);
}

static bool s_at_home;                        // last job ended resting at Home (idle life may breathe there)
static bool go_home(void)
{
    uint16_t p[NSERVO];
    pose(p, g_cfg.home, 0, hand());
    bool ok = travel(p, g_cfg.move_ms);
    s_at_home = ok;
    return ok;
}

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
    if (!travel(p, g_cfg.move_ms)) return;
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

// the moves alone (Robot School runs them while it talks); do_* add the line and the state
static bool wave_moves(void)
{
    uint16_t p[NSERVO];
    pose(p, g_cfg.home, 0.6f, g_cfg.grip_open);              // a bit raised
    if (!go(p, g_cfg.move_ms)) return false;
    for (int i = 0; i < 4; i++) {
        p[1] = clamp_pos(g_cfg.home[1] + (i % 2 ? -170 : 170));  // wrist rotate
        p[0] = i % 2 ? g_cfg.grip_open : g_cfg.grip_close;        // hand opens and closes
        if (!beat(p, 380)) return false;    // 340-unit swing: faster than ~380 ms outruns the servo
    }
    s_holding = false;                       // the hand opened on the way
    return go_home();
}

static void do_wave(void)
{
    fx_say(SND_WAVE);
    set("waving", "hello!");
    if (!wave_moves()) return;
    set("idle", "waved");
}

static bool bow_moves(void)
{
    uint16_t p[NSERVO];
    pose(p, g_cfg.home, -0.8f, hand());                      // opposite of lift = lean forward/down
    if (!go(p, 900)) return false;
    if (!wait_ms(500)) return false;
    return go_home();
}

static void do_bow(void)
{
    fx_say(SND_BOW);
    set("bowing", "thank you!");
    if (!bow_moves()) return;
    set(s_holding ? "holding" : "idle", "bowed");
}

static bool dance_moves(void)
{
    uint16_t p[NSERVO];
    static const int8_t base[8] = {1, -1, 1, -1, 2, -2, 0, 0}, wrist[8] = {1, 1, -1, -1, 1, -1, 1, 0};
    for (int i = 0; i < 8; i++) {
        pose(p, g_cfg.home, (i % 2) ? 0.5f : 0.2f, (i % 2) ? g_cfg.grip_close : g_cfg.grip_open);
        p[5] = clamp_pos(g_cfg.home[5] + 90 * base[i]);
        p[1] = clamp_pos(g_cfg.home[1] + 140 * wrist[i]);
        if (!beat(p, 330)) return false;
    }
    s_holding = false;
    return go_home();
}

// music playing in the room? dance on its beat: 16 beats, each move landing on the beat, with no tune of its own
static bool beat_moves(int period_ms, int64_t next_us)
{
    uint16_t p[NSERVO];
    static const int8_t base[8] = {1, -1, 1, -1, 2, -2, 1, 0}, wrist[8] = {1, -1, -1, 1, 1, -1, 1, 0};
    int move_ms = period_ms * 8 / 10;
    for (int i = 0; i < 16; i++) {
        int64_t start = next_us + (int64_t)i * period_ms * 1000 - (int64_t)move_ms * 1000;   // arrive on the beat
        int64_t wait = start - esp_timer_get_time();
        if (wait > 0 && !wait_raw((int)(wait / 1000))) return false;
        int k = i % 8;
        pose(p, g_cfg.home, (i % 2) ? 0.5f * mood_k() : 0.15f, (i % 4 < 2) ? g_cfg.grip_close : g_cfg.grip_open);
        p[5] = clamp_pos(g_cfg.home[5] + (int)(80 * base[k] * mood_k()));
        p[1] = clamp_pos(g_cfg.home[1] + 130 * wrist[k]);
        arm_move(p, move_ms);
    }
    if (!wait_ms(move_ms)) return false;
    s_holding = false;
    return go_home();
}

static void do_dance(void)
{
    int period;
    int64_t next;
    fx_say(SND_DANCE);
    if (voice_beat(&period, &next)) {
        set("grooving", "on the beat, %d bpm", 60000 / period);
        if (!beat_moves(period, next)) return;
    } else {
        set("dancing", "♪");
        if (!dance_moves()) return;
    }
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
    if (kids_score("tower", level)) celebrate_moves();   // a new best: a victory lap
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

// ---- talking gestures: while Muffin talks, little nods land on the loud bits of its own voice (fx_talk_level),
// with now and then a sway of the base and a turn of the head. Small, around whatever pose it's in.
static struct { uint16_t base[NSERVO]; float smooth; int64_t nod_us; int nods; bool out; } s_talk;

static void talk_begin(void) { arm_last(s_talk.base); s_talk.smooth = 0; s_talk.nod_us = 0; s_talk.out = false; }

static void talk_step(void)
{
    int64_t now = esp_timer_get_time();
    float lvl = fx_talk_level();
    bool onset = lvl > 0.22f && lvl > s_talk.smooth * 1.5f && now - s_talk.nod_us > 280000;
    s_talk.smooth = s_talk.smooth * 0.7f + lvl * 0.3f;
    uint16_t p[NSERVO];
    memcpy(p, s_talk.base, sizeof p);
    if (onset) {
        int dir = (s_talk.nods++ & 1) ? 1 : -1;
        p[2] = clamp_pos(p[2] + dir * (int)((10 + 16 * lvl) * mood_k()));        // a nod
        if (s_talk.nods % 4 == 0) {                                             // emphasis: sway and turn
            p[5] = clamp_pos(p[5] + dir * (int)(14 * mood_k()));
            p[1] = clamp_pos(p[1] - dir * 18);
        }
        arm_move(p, 150);
        s_talk.nod_us = now;
        s_talk.out = true;
    } else if (s_talk.out && now - s_talk.nod_us > 170000) {
        arm_move(p, 260);                                                       // and back
        s_talk.out = false;
    }
}

static void talk_end(void) { if (s_talk.out) { arm_move(s_talk.base, 260); s_talk.out = false; } }

// talk along until the sentence is over (replaces wait_quiet in lessons)
static bool talk_along(void)
{
    if (!s_anim) return wait_quiet();
    talk_begin();
    for (int t = 0, quiet = 0; t < 30000; t += 40) {
        if (s_abort) return false;
        quiet = fx_speaking() ? 0 : quiet + 40;
        if (quiet >= 220) break;
        talk_step();
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    talk_end();
    return wait_raw(120);
}

// ---- celebrations: rank-ups, records, finishing the course. Up, a spin each way with bells, jazz hands, a bow.
static bool celebrate_moves(void)
{
    uint16_t p[NSERVO];
    float mk = mood_k() < 1 ? 1 : mood_k();          // even a grumpy robot celebrates properly
    pose(p, g_cfg.home, 0.5f, g_cfg.grip_open);
    if (!travel(p, 600)) return false;
    for (int i = 0; i < 2; i++) {
        int d = i ? -1 : 1;
        p[5] = clamp_pos(g_cfg.home[5] + d * (int)(200 * mk));
        p[1] = clamp_pos(g_cfg.home[1] + d * 120);
        if (!go(p, 520)) return false;
        fx_event(FX_BELL);
    }
    p[5] = g_cfg.home[5]; p[1] = g_cfg.home[1];
    pose(p, g_cfg.home, 0.65f, g_cfg.grip_open);
    if (!go(p, 400)) return false;
    for (int i = 0; i < 4; i++) { p[0] = i & 1 ? g_cfg.grip_open : g_cfg.grip_close; if (!go(p, 180)) return false; }   // jazz hands
    fx_event(FX_BELL);
    s_holding = false;
    return bow_moves();
}

static void do_celebrate(void)
{
    if (s_holding) return;                   // don't fling what it's holding
    set("celebrating", "woo!");
    fx_say(SND_CELEBRATE);
    if (celebrate_moves()) set("idle", "celebrated");
}

// ---- Robot School (school.c): a lesson part = its sentences from `first` up to the next kid's turn (or the
// end), each with the arm act it names (SCHOOL_STEP in sounds_gen.h, written in tools/school_lessons.py).
// The act runs while the sentence plays. When school.c says time's up, it finishes the sentence and stops.
static volatile int s_lesson_next = -1;
int pick_lesson_next(void) { return s_lesson_next; }

static bool school_act(int a)
{
    uint16_t p[NSERVO];
    pose(p, g_cfg.home, 0, hand());
    switch (a) {
    case SA_HOME: {                               // keeps the hand as it is ("carry it home")
        uint16_t c[NSERVO];
        arm_last(c);
        pose(p, g_cfg.home, 0, c[0]);
        return (s_at_home = travel(p, g_cfg.move_ms));
    }
    case SA_LOOK:     return g_cfg.cam_on_head ? look_at(0) : true;
    case SA_BASE:     return wiggle(5, 120);
    case SA_SHOULDER: return wiggle(4, 60);
    case SA_ELBOW:    return wiggle(3, 60);
    case SA_WRIST:    return wiggle(2, 90);
    case SA_TWIST:    return wiggle(1, 160);
    case SA_GRIP:     if (s_holding) return true;           // don't drop what it's holding to make a point
                      return grip(g_cfg.grip_open) && grip(g_cfg.grip_close) && grip(g_cfg.grip_open);
    case SA_OPEN:     if (s_holding) return true;
                      return grip(g_cfg.grip_open);
    case SA_CLOSE:    if (s_holding) return true;
                      return grip(g_cfg.grip_close);
    case SA_OUT:      pose(p, g_cfg.give, 0, hand()); return travel(p, g_cfg.move_ms);
    case SA_JERKY:                                            // constant speed, hard stops
        p[5] = clamp_pos(g_cfg.home[5] + 150); arm_move_lin(p, 260); if (!wait_raw(420)) return false;
        p[5] = clamp_pos(g_cfg.home[5] - 150); arm_move_lin(p, 380); if (!wait_raw(540)) return false;
        p[5] = g_cfg.home[5];                  arm_move_lin(p, 260); return wait_raw(420);
    case SA_SMOOTH:                                           // eased
        p[5] = clamp_pos(g_cfg.home[5] + 150); if (!go(p, 900)) return false;
        p[5] = clamp_pos(g_cfg.home[5] - 150); if (!go(p, 1300)) return false;
        p[5] = g_cfg.home[5];                  return go(p, 900);
    case SA_FAST:
        p[5] = clamp_pos(g_cfg.home[5] + 150); if (!go(p, 350)) return false;
        p[5] = g_cfg.home[5];                  return go(p, 350);
    case SA_SLOW:
        p[5] = clamp_pos(g_cfg.home[5] + 150); if (!go(p, 2000)) return false;
        p[5] = g_cfg.home[5];                  return go(p, 2000);
    case SA_WAVE:     return s_holding ? true : wave_moves();     // these open the hand: never while holding
    case SA_BOW:      return bow_moves();
    case SA_DANCE:    return s_holding ? true : dance_moves();
    case SA_BELL:     fx_event(FX_BELL); return wait_ms(300);
    case SA_JEWELS:
        for (int j = 0; j < 4; j++) { fx_show(j); if (!wait_ms(450)) return false; }
        return true;
    case SA_SCAN:
        if (!g_cfg.cam_on_head) return wiggle(5, 200);
        return look_at(g_cfg.scan[1]) && look_at(g_cfg.scan[2]) && look_at(0);
    case SA_JOINTS: {
        static const int8_t idx[5] = {5, 4, 3, 2, 1};
        static const int16_t amt[5] = {120, 60, 60, 90, 160};
        for (int k = 0; k < 5; k++) if (!wiggle(idx[k], amt[k])) return false;
        return s_holding || (grip(g_cfg.grip_open) && grip(g_cfg.grip_close) && grip(g_cfg.grip_open));
    }
    case SA_CHEER:    fx_play(SND_S_CHEER); return true;
    default:          return true;
    }
}

static void do_lesson(int slot)
{
    bool explain = slot & LESSON_EXPLAIN;      // answering a question: the whole lesson, skipping the kid's turns
    int first = slot & 0xFFFF, end = (slot >> 16) & 0x3FFF;
    if (end > SCHOOL_STEPS) end = SCHOOL_STEPS;
    s_lesson_next = first;
    set("teaching", "school, step %d", first);
    for (int i = first; i < end; i++) {
        if (explain && SCHOOL_STEP[i].turn != K_NONE) continue;
        fx_say(SCHOOL_STEP[i].g);
        if (!school_act(SCHOOL_STEP[i].act) || !talk_along()) return;   // stopped: s_lesson_next stays put
        s_lesson_next = i + 1;
        if ((!explain && SCHOOL_STEP[i].turn != K_NONE) || school_wrap_due()) break;
    }
    if (explain && !go_home()) return;
    set(s_holding ? "holding" : "idle", "school, step %d", s_lesson_next);
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

// ---- idle life: between jobs, resting at Home. Breathes and glances around while someone's been around in the
// last LIFE_US, then goes still; perks up when called; head down with slow breaths while School has it asleep.
// Never while holding something, limp, or after the app has moved the arm by hand (until the next job).
#define LIFE_US 90000000LL
static int64_t s_job_end_us, s_life_next;
static volatile int64_t s_attn_us;
static volatile bool s_perk;
static int s_life_step;
static bool s_asleep_pose;

void pick_attention(void) { s_attn_us = esp_timer_get_time(); s_perk = true; }

// which way the last voice came from (voice.c, -1..1), and whether its ears are open right now
#define FACE_MAX 140                          // base units at full left/right (~35°)
static volatile float s_face;
static volatile bool s_listen;
static bool s_listen_pose;
void pick_face(float d) { s_face = d < -1 ? -1 : d > 1 ? 1 : d; s_attn_us = esp_timer_get_time(); }
void pick_listening(bool on) { s_listen = on; }

// where it rests: Home, turned toward the last voice, a little slumped when grumpy and perky when delighted;
// leaning in with its head cocked while it listens
static void rest_pose(uint16_t p[NSERVO], float lift_k)
{
    int md = chat_mood();
    pose(p, g_cfg.home, lift_k + (md <= -2 ? -0.08f : md >= 2 ? 0.04f : 0), hand());
    p[5] = clamp_pos(g_cfg.home[5] + (int)(s_face * FACE_MAX));
    if (s_listen_pose) {
        for (int i = 1; i < NSERVO; i++) p[i] = clamp_pos(p[i] + g_reach[i] / 4);   // lean in
        p[1] = clamp_pos(p[1] + 45);                                                // head cocked
    }
}

// boredom: nobody around for LIFE_US -> a stretch, a yawn, a droop into a nap with slow breaths; after NAP_US of
// napping it just holds the nap pose. Its name (or its ears opening) wakes it straight up.
#define NAP_US (10 * 60 * 1000000LL)
enum { B_AWAKE, B_STRETCH_UP, B_STRETCH_SWAY, B_YAWN, B_NAP, B_DEEP };
static int s_bored;
static int64_t s_nap_us;
static bool s_talk_idle;
static pink_t s_pk1, s_pk2, s_pk3;

static void idle_life(void)
{
    int64_t now = esp_timer_get_time(), last = s_job_end_us > s_attn_us ? s_job_end_us : s_attn_us;
    if (!s_anim || s_abort || !s_at_home || s_holding || !arm_torque_on() || !s_job_end_us || arm_manual_us() > s_job_end_us) {
        s_perk = false;                      // s_abort: someone said stop, so nothing moves until the next job
        return;
    }
    uint16_t p[NSERVO];
    bool asleep = school_asleep();
    if (asleep != s_asleep_pose) {           // School: nodding off for the night, or waking up
        s_asleep_pose = asleep;
        s_life_step = 0;
        s_bored = B_AWAKE;
        pose(p, g_cfg.home, asleep ? -0.33f : 0, hand());
        arm_move(p, asleep ? 2500 : 900);
        s_life_next = now + 2600000;
        return;
    }
    if (asleep) {                            // slow, small breaths
        if (now < s_life_next) return;
        s_life_step ^= 1;
        pose(p, g_cfg.home, s_life_step ? -0.28f : -0.34f, hand());
        arm_move(p, 3400);
        s_life_next = now + 3600000;
        return;
    }
    if (s_perk) {                            // someone said its name: turns toward them, head up, then relaxes
        s_perk = false;
        int ms = s_bored >= B_YAWN ? 550 : 320;   // waking from a nap takes a moment longer
        s_bored = B_AWAKE;
        rest_pose(p, 0.18f * mood_k());
        uint16_t now_p[NSERVO];
        arm_last(now_p);
        if (abs((int)p[5] - (int)now_p[5]) > 120) ms = 600;   // a big turn toward a voice: not faster than the servo likes
        arm_move(p, mood_ms(ms));
        s_life_step = 1;
        s_life_next = now + 600000;
        return;
    }
    if (s_listen != s_listen_pose) {         // ears open: lean in, head cocked; closed: sit back
        s_listen_pose = s_listen;
        if (s_listen) { s_bored = B_AWAKE; s_attn_us = now; }
        rest_pose(p, 0);
        arm_move(p, jitter(450));
        s_life_step = 0;
        s_life_next = now + 700000;
        return;
    }
    if (fx_speaking() && s_bored == B_AWAKE) {   // it's talking: nod along to its own voice
        if (!s_talk_idle) { s_talk_idle = true; talk_begin(); }
        talk_step();
        return;
    }
    if (s_talk_idle) { s_talk_idle = false; talk_end(); s_life_next = now + 300000; return; }
    if (now < s_life_next) return;

    if (now - last > LIFE_US) {              // nobody around: get bored
        switch (s_bored) {
        case B_AWAKE:                        // a big stretch up...
            s_face = 0;
            pose(p, g_cfg.home, 0.4f, g_cfg.grip_close);
            p[5] = clamp_pos(g_cfg.home[5] + 60);
            p[1] = clamp_pos(g_cfg.home[1] + 60);
            arm_move(p, 1300);
            s_bored = B_STRETCH_UP;
            s_life_next = now + 1500000;
            return;
        case B_STRETCH_UP:                   // ...and over to the other side
            pose(p, g_cfg.home, 0.4f, g_cfg.grip_open);
            p[5] = clamp_pos(g_cfg.home[5] - 60);
            p[1] = clamp_pos(g_cfg.home[1] - 60);
            arm_move(p, 1100);
            s_bored = B_STRETCH_SWAY;
            s_life_next = now + 1300000;
            return;
        case B_STRETCH_SWAY:                 // a yawn, and the head sinks
            fx_say(SND_YAWN);
            pose(p, g_cfg.home, -0.24f, hand());
            arm_move(p, 2800);
            s_bored = B_YAWN;
            s_life_next = now + 3000000;
            return;
        case B_YAWN:
            s_bored = B_NAP;
            s_nap_us = now;
            /* fall through */
        case B_NAP:                          // napping: slow breaths
            if (now - s_nap_us > NAP_US) { pose(p, g_cfg.home, -0.24f, hand()); arm_move(p, 2000); s_bored = B_DEEP; return; }
            s_life_step ^= 1;
            pose(p, g_cfg.home, s_life_step ? -0.19f : -0.25f, hand());
            arm_move(p, jitter(4200));
            s_life_next = now + jitter(4400) * 1000LL;
            return;
        default:                             // deep nap: hold still
            s_life_next = now + 1000000;
            return;
        }
    }
    if (s_bored != B_AWAKE) {                // shouldn't happen (attention resets it), but wake cleanly
        s_bored = B_AWAKE;
        rest_pose(p, 0);
        arm_move(p, 600);
        s_life_next = now + 700000;
        return;
    }
    int md = chat_mood();
    float w1 = pink_next(&s_pk1, esp_random()), w2 = pink_next(&s_pk2, esp_random()), w3 = pink_next(&s_pk3, esp_random());
    if (!s_life_step && !s_listen_pose && esp_random() % (md >= 2 ? 3 : md <= -2 ? 9 : 5) == 0) {   // a glance
        rest_pose(p, 0.05f);
        p[5] = clamp_pos(p[5] + (esp_random() & 1 ? 1 : -1) * (int)((40 + esp_random() % 50) * mood_k()));
        arm_move(p, mood_ms(jitter(900)));
        s_life_step = 1;
        s_life_next = now + jitter(2400) * 1000LL;
        return;
    }
    // breathe in, breathe out, never quite the same twice: depth, pace and a little drift wander on pink noise
    s_life_step ^= 1;
    float depth = (s_listen_pose ? 0.03f : 0.07f) * mood_k() * (1.0f + 0.4f * w1);
    rest_pose(p, s_life_step ? depth : 0);
    if (!s_listen_pose) {
        p[5] = clamp_pos(p[5] + (int)(10 * w2));
        p[2] = clamp_pos(p[2] + (int)(8 * w3));
    }
    int ms = mood_ms((int)(2200 * (1.0f + 0.25f * w2)));
    arm_move(p, ms);
    s_life_next = now + (ms + 100) * 1000LL;
}

static void task(void *arg)
{
    job_t j;
    for (;;) {
        if (xQueueReceive(s_q, &j, pdMS_TO_TICKS(40)) != pdTRUE) { idle_life(); continue; }
        s_abort = false;
        s_busy = true;
        s_at_home = false;
        s_life_step = 0;
        s_asleep_pose = false;
        s_listen_pose = false;
        s_bored = 0;
        s_talk_idle = false;
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
        case OP_LESSON: do_lesson(j.slot); break;
        case OP_CELEBRATE: do_celebrate(); break;
        case OP_HOME:
            fx_maybe(SND_HOME, 50);
            set("moving", "home");
            if (go_home()) set(s_holding ? "holding" : "idle", "home");
            break;
        case OP_REACT:                       // slot: 1 = good, 0 = bad, +2 = "hmm" first, 4 = only "hmm"
            if ((j.slot & 6) && !think()) break;
            if (!(j.slot & 4) && !react(j.slot & 1)) break;
            set(s_holding ? "holding" : "idle", "%s", j.slot & 4 ? "hmm" : j.slot & 1 ? "yes!" : "aw");
            break;
        case OP_OPEN:  fx_say(strcmp(s_state, "helping") ? SND_OK : SND_RELEASED); set("moving", "open"); if (grip(g_cfg.grip_open)) { s_holding = false; set("idle", "hand open"); } break;
        case OP_CLOSE: fx_say(strcmp(s_state, "helping") ? SND_OK : SND_HELP_HOLD); set("moving", "close"); if (grip(g_cfg.grip_close)) { s_holding = true; set("holding", "hand closed"); } break;
        }
        if (s_abort) set("stopped", "stopped");
        s_job_end_us = esp_timer_get_time();
        s_busy = false;
        bool show = j.op == OP_WAVE || j.op == OP_BOW || j.op == OP_DANCE || j.op == OP_BELL || j.op == OP_PLAY || j.op == OP_STACK;
        if (show && !s_abort) chat_show_done(j.op, j.slot);
    }
}

void pick_init(void)
{
    s_q = xQueueCreate(1, sizeof(job_t));
    nvs_handle_t h;
    uint8_t a;
    if (nvs_open("xarm", NVS_READONLY, &h) == ESP_OK) { if (nvs_get_u8(h, "anim", &a) == ESP_OK) s_anim = a; nvs_close(h); }
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
bool pick_idle(void) { return !s_busy && !uxQueueMessagesWaiting(s_q); }
bool pick_holding(void) { return s_holding; }
void pick_status(char *st, size_t ns, char *d, size_t nd)
{
    strlcpy(st, s_state, ns);
    strlcpy(d, s_detail, nd);
}
