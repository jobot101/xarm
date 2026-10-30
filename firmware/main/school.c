// school.c — Muffin's Robot School (see school.h). One task runs the class as a plain script; the voice
// task feeds it answers through school_filter() / school_allow() / school_note_stop().
#include "school.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "core.h"
#include "fx.h"
#include "pick.h"
#include "arm.h"
#include "kids.h"
#include "voice.h"
#include "cloud.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "school";

#define GUEST          KIDS_MAX     // progress slot for "nobody said their name"
#define EXTRA_S        600          // grown-up passphrase: ten more minutes
#define ANSWER_S       20           // waiting for an answer or a "your turn" command
#define AGAIN_S        12           // after asking again
#define FREE_IDLE_S    180          // free play with nobody talking: pause the clock
#define LESSON_MIN_S   240          // don't start a lesson with less than this left
#define MORE_MIN_S     360          // offer another lesson only with this much left
#define MAX_LESSONS    3            // new lessons a day: spacing beats cramming
#define SILENT_PAUSE   2            // unanswered prompts in a row = they walked away: pause
#define ASLEEP_SAY_US  60000000LL   // while asleep, say so at most once a minute
#define WAKE_US        8000000LL    // power-on: settle this long, then wake up and say hi
#define INVITE_US      240000000LL  // between classes, nobody talking this long: invite them back
#define INVITE_MAX     3            // invites in a row before it lets itself get bored and nap
#define OPEN_US        20000000LL   // after a hello or invite, ears stay open this long (no "Hey Muffin")

// ---------------------------------------------------------------- progress, per kid (+ guest)
typedef struct {
    int32_t  day;            // day of the last class (school_today())
    uint16_t used_s;         // seconds of class that day
    uint16_t extra_s;        // grown-up bonus that day
    uint16_t streak;         // days in a row
    uint16_t stars;          // all time
    uint8_t  next;           // next lesson; SCHOOL_COUNT = finished the course
    uint8_t  lessons_today;
    uint8_t  stars_today;
    uint8_t  warmed;         // warm-up done today
    uint8_t  missed[24];     // SCHOOL_QUIZ questions answered wrong and not put right since
} prog_t;
_Static_assert(SCHOOL_QUIZ_COUNT <= 24 * 8, "missed[] too small for the quiz");
_Static_assert(SCHOOL_COUNT < 255, "next is a uint8_t");

#define PROG_MAGIC 0x53434831u     // "SCH1": bump if prog_t changes
static struct { uint32_t magic; prog_t p[KIDS_MAX + 1]; } s_save;
#define P (s_save.p)
static SemaphoreHandle_t s_mx;

static bool s_on = true;
static bool s_tap = true;                    // taps only (default): Muffin's ears are off, the kid answers on the Learning page
static uint8_t s_minutes = CONFIG_XARM_SCHOOL_MIN;
static char s_pass[48];

static void lock(void) { xSemaphoreTake(s_mx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mx); }

static void save(void)                       // call locked
{
    nvs_handle_t h;
    if (nvs_open_from_partition("store", "school", NVS_READWRITE, &h) != ESP_OK) return;
    s_save.magic = PROG_MAGIC;
    nvs_set_blob(h, "prog", &s_save, sizeof s_save);
    nvs_commit(h);
    nvs_close(h);
}

static void load(void)
{
    nvs_handle_t h;
    size_t len = sizeof s_save;
    memset(&s_save, 0, sizeof s_save);
    if (nvs_open_from_partition("store", "school", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "prog", &s_save, &len) != ESP_OK || len != sizeof s_save || s_save.magic != PROG_MAGIC)
            memset(&s_save, 0, sizeof s_save);
        nvs_close(h);
    }
    for (int k = 0; k <= KIDS_MAX; k++) if (P[k].next > SCHOOL_COUNT) P[k].next = 0;
    if (nvs_open("xarm", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "sc_on", &v) == ESP_OK) s_on = v;
        if (nvs_get_u8(h, "sc_min", &v) == ESP_OK && v >= 5) s_minutes = v;
        if (nvs_get_u8(h, "sc_tap", &v) == ESP_OK) s_tap = v;
        size_t n = sizeof s_pass;
        if (nvs_get_str(h, "sc_pass", s_pass, &n) != ESP_OK) s_pass[0] = 0;
        nvs_close(h);
    }
}

static void save_settings(void)
{
    nvs_handle_t h;
    if (nvs_open("xarm", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "sc_on", s_on);
    nvs_set_u8(h, "sc_min", s_minutes);
    nvs_set_u8(h, "sc_tap", s_tap);
    nvs_set_str(h, "sc_pass", s_pass);
    nvs_commit(h);
    nvs_close(h);
}

void school_kid_reset(int kid)
{
    if (kid < 0 || kid > KIDS_MAX || !s_mx) return;
    lock();
    memset(&P[kid], 0, sizeof P[kid]);
    save();
    unlock();
}

const char *school_pass(void) { return s_pass; }

// ---------------------------------------------------------------- the clock: what day is it?
static i2c_master_dev_handle_t s_rtc;
static volatile bool s_clock_ok;
static const char *s_clock_src = "none";
static int32_t s_boot_day;                   // no clock at all: each boot is its own (negative) day

static int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
static uint8_t tobcd(int v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

// PCF85063 at 0x51, kept in UTC: 0x04 seconds (bit 7 = the oscillator stopped, time lost) .. 0x0A year
static void rtc_read(void)
{
    uint8_t reg = 0x04, r[7];
    if (!s_rtc || i2c_master_transmit_receive(s_rtc, &reg, 1, r, sizeof r, 50) != ESP_OK) { ESP_LOGW(TAG, "no RTC"); return; }
    if (r[0] & 0x80) { ESP_LOGW(TAG, "RTC lost the time (no backup battery?)"); return; }
    int y = 2000 + bcd(r[6]), mo = bcd(r[5] & 0x1F), d = bcd(r[3] & 0x3F);
    if (y < 2025 || mo < 1 || mo > 12 || d < 1 || d > 31) return;
    time_t t = (time_t)civil_days(y, mo, d) * 86400 + bcd(r[2] & 0x3F) * 3600 + bcd(r[1] & 0x7F) * 60 + bcd(r[0] & 0x7F);
    struct timeval tv = { .tv_sec = t };
    settimeofday(&tv, NULL);
    s_clock_ok = true;
    s_clock_src = "rtc";
    ESP_LOGI(TAG, "time from the RTC");
}

static void rtc_write(time_t t)
{
    if (!s_rtc) return;
    struct tm g;
    gmtime_r(&t, &g);
    uint8_t w[8] = { 0x04, tobcd(g.tm_sec), tobcd(g.tm_min), tobcd(g.tm_hour), tobcd(g.tm_mday), (uint8_t)g.tm_wday,
                     tobcd(g.tm_mon + 1), tobcd(g.tm_year % 100) };   // writing the seconds clears "time lost"
    i2c_master_transmit(s_rtc, w, sizeof w, 50);
}

static void on_sntp(struct timeval *tv)
{
    s_clock_ok = true;
    s_clock_src = "internet";
    rtc_write(tv->tv_sec);
}

void school_clock_start(void)
{
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    c.sync_cb = on_sntp;
    if (esp_netif_sntp_init(&c) != ESP_OK) ESP_LOGW(TAG, "no internet time");
}

static int32_t today(void)
{
    time_t now = time(NULL);
    if (!s_clock_ok || now < 1700000000) return s_boot_day;
    struct tm t;
    localtime_r(&now, &t);
    return civil_days(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}

// ---------------------------------------------------------------- state shared with the voice task
typedef enum { ST_IDLE, ST_RUN, ST_ASLEEP } st_t;
enum { E_NONE, E_TF, E_YN, E_TURN, E_KID, E_STOP, E_FREE };
enum { A_NONE, A_TRUE, A_FALSE, A_YES, A_NO, A_TURN_OK, A_KID, A_STOP };

static volatile st_t s_st = ST_IDLE;
static volatile int s_kid = -1;              // whose class it is (GUEST = no name), -1 = nobody yet
static volatile bool s_start, s_wrap, s_paused;
static volatile int s_want_kid = -1;         // named when they started it
static volatile int s_expect = E_NONE, s_turn_kind, s_ans, s_ans_kid;
static volatile uint32_t s_stops;
static volatile int64_t s_heard_us, s_asleep_said;
static int32_t s_asleep_day;
static int s_asleep_kid = -1;
static bool s_content;                       // lessons are on the card
static int s_silent;                         // unanswered prompts in a row
static bool s_said_tf_how;
static volatile int s_now_lesson = -1;      // the lesson being taught right now (Learning page)
static volatile int s_want_lesson = -1;     // the kid tapped Start lesson on this one (Learning page)
static volatile int s_q = -1;               // the true/false question being asked (Learning page pop-up)

static int limit_s(int k) { return s_minutes * 60 + P[k].extra_s; }

static bool has_time(int k)                  // does kid k have class time left today? (locked)
{
    if (k < 0 || k > KIDS_MAX) return false;
    if (P[k].day != today()) return true;
    return P[k].used_s < limit_s(k);
}

static int left_s(int k) { return P[k].day != today() ? limit_s(k) : limit_s(k) - P[k].used_s; }

static int default_kid(void)
{
    int c = kids_current();
    if (c >= 0) return c;
    if (kids_count() == 1) for (int i = 0; i < KIDS_MAX; i++) if (kids_get(i)) return i;
    return kids_count() ? -1 : GUEST;
}

// ---------------------------------------------------------------- time keeping (school task)
static int64_t s_tick_us, s_saved_us, s_acc;

static void tick(void)
{
    int64_t now = esp_timer_get_time();
    int64_t dt = s_tick_us ? now - s_tick_us : 0;
    s_tick_us = now;
    int k = s_kid;
    if (s_st != ST_RUN || k < 0 || s_paused) return;
    s_acc += dt;
    lock();
    while (s_acc >= 1000000) { s_acc -= 1000000; if (P[k].used_s < 65000) P[k].used_s++; }
    if (P[k].used_s >= limit_s(k)) s_wrap = true;
    if (now - s_saved_us > 30000000) { s_saved_us = now; save(); }
    unlock();
}

static void nap(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); tick(); }

// one line, start to finish (queued lines go stale after 2.5 s, so School never stacks them)
static void say(int g)
{
    if (!fx_has(g)) return;
    fx_say(g);
    nap(40);
    for (int t = 0; t < 30000 && fx_speaking(); t += 50) nap(50);
}

#define TAP(g) (s_tap && fx_has(g##_TAP) ? g##_TAP : g)   // the tap-mode wording of a line, if it's on the card

static void say_n(int first, int n) { if (n >= 1 && n <= SND_N_NUMBERS) say(first + n - 1); }

// run an arm job to the end; false if someone said stop during it
static bool job(op_t op, int slot)
{
    uint32_t stops = s_stops;
    char msg[64];
    for (int t = 0; t < 3000 && !pick_idle(); t += 50) nap(50);        // something still finishing
    if (!pick_run(op, slot, msg, sizeof msg)) return s_stops == stops;
    nap(120);
    for (int quiet = 0; quiet < 2;) { nap(60); quiet = pick_idle() ? quiet + 1 : 0; }
    return s_stops == stops;
}

// wait for what we asked for; A_NONE on timeout. Keeps Muffin's ears open (no "Hey Muffin" needed).
// what each kind of turn sounds like, for the cloud ears' hint
static const char *turn_words(int k)
{
    switch (k) {
    case K_UP: return "A little up.";              case K_DOWN: return "A little down.";
    case K_LEFT: return "A little left.";          case K_RIGHT: return "A little right.";
    case K_TURN: return "Turn left. Turn right.";  case K_TILT: return "Tilt up. Tilt down.";
    case K_OPEN: return "Open your hand.";         case K_CLOSE: return "Close your hand.";
    case K_BELL: return "Ring the bell.";          case K_DANCE: return "Let's dance.";
    case K_WAVE: return "Wave hello.";             case K_BOW: return "Take a bow.";
    case K_HOME: return "Go home.";                case K_STOP: return "Stop!";
    case K_FETCH: return "Grab it. Close your hand."; case K_GIVEBACK: return "Let go. Open your hand.";
    default: return "A little up. A little left. Turn left. Tilt up.";
    }
}

static void hint_for(int expect)
{
    char h[160] = "";
    if (expect == E_TF) strlcpy(h, "That's true. That's false.", sizeof h);
    else if (expect == E_YN) strlcpy(h, "Yes please. No thanks.", sizeof h);
    else if (expect == E_TURN || expect == E_STOP) strlcpy(h, turn_words(expect == E_STOP ? K_STOP : s_turn_kind), sizeof h);
    else if (expect == E_KID) {
        for (int i = 0; i < KIDS_MAX; i++) if (kids_get(i)) { strlcat(h, "Hi, I'm ", sizeof h); strlcat(h, kids_get(i)->name, sizeof h); strlcat(h, ". ", sizeof h); }
    }
    cloud_hint(h);
}

static int await(int expect, int secs)
{
    uint32_t stops = s_stops;
    s_ans = A_NONE;
    s_expect = expect;
    hint_for(expect);
    int64_t until = esp_timer_get_time() + (int64_t)secs * 1000000, eng = 0;
    while (esp_timer_get_time() < until && s_ans == A_NONE && !s_wrap && s_on) {
        if (s_stops != stops && expect != E_STOP) { s_ans = A_STOP; break; }
        if (esp_timer_get_time() - eng > 4000000 && !fx_speaking()) { voice_engage(); eng = esp_timer_get_time(); }
        nap(50);
    }
    int a = s_ans;
    s_expect = E_NONE;
    cloud_hint("");
    if (a != A_NONE) s_silent = 0;
    return a;
}

static void pause_class(void)                // kid said stop or walked away: clock stops, resume on next word
{
    s_paused = true;
    s_expect = E_NONE;
    voice_hold_open(false);
    lock(); save(); unlock();
    s_st = ST_IDLE;
    ESP_LOGI(TAG, "paused");
}

static bool gone(void) { return ++s_silent >= SILENT_PAUSE; }

// ---------------------------------------------------------------- the class
static void set_missed(int q, bool missed)
{
    lock();
    if (missed) P[s_kid].missed[q >> 3] |= 1 << (q & 7);
    else P[s_kid].missed[q >> 3] &= ~(1 << (q & 7));
    unlock();
}

static void star(void)
{
    lock();
    if (P[s_kid].stars < 65000) P[s_kid].stars++;
    if (P[s_kid].stars_today < 250) P[s_kid].stars_today++;
    unlock();
}

// one true/false question: 1 right, 0 wrong or unanswered, -1 class interrupted
static int question(int q)
{
    say(SCHOOL_QUIZ[q].q);
    if (!s_said_tf_how) { s_said_tf_how = true; say(TAP(SND_SC_TF_HOW)); }
    s_q = q;
    int a = await(E_TF, ANSWER_S);
    if (a == A_NONE && !s_wrap) { say(TAP(SND_SC_TF_HOW)); a = await(E_TF, AGAIN_S); }
    s_q = -1;
    if (a == A_STOP || s_wrap) return -1;
    if (a == A_NONE) {
        say(SND_SC_NO_ANSWER);
        say(SCHOOL_QUIZ[q].why);
        set_missed(q, true);
        return gone() ? -1 : 0;
    }
    bool right = (a == A_TRUE) == (SCHOOL_QUIZ[q].truth != 0);
    job(OP_REACT, 4);                        // "hmm": a thinking look before the verdict
    char m[48];
    pick_run(OP_REACT, right, m, sizeof m);  // a proud bob, or a little slump, while it answers
    say(right ? SND_RIGHT : SND_WRONG);
    say(SCHOOL_QUIZ[q].why);
    set_missed(q, !right);
    if (right) { star(); say(SND_SC_STAR); }
    return right;
}

// warm-up / review: n questions from lessons already learned, missed ones first
static bool warmup(int n)
{
    int learned = P[s_kid].next >= SCHOOL_COUNT ? SCHOOL_QUIZ_COUNT : SCHOOL[P[s_kid].next].quiz;
    if (!learned) return true;
    int last = -1;
    for (int i = 0; i < n && i < learned; i++) {
        int q = recap_pick(P[s_kid].missed, learned, esp_random(), last);
        if (q < 0) break;
        last = q;
        if (question(q) < 0) return false;
    }
    lock(); P[s_kid].warmed = 1; unlock();
    return true;
}

// the kid's turn after a step that asks for one; false if the class got interrupted
static void settle(void)                     // the command the router just let through: let it finish
{
    nap(300);
    for (int quiet = 0; quiet < 2;) { nap(60); quiet = pick_idle() ? quiet + 1 : 0; }
}

// "hand me something": the hand is open toward the kid (act handoff). They put it in and tap Clamp (K_CLOSE);
// Muffin checks the jaws stopped on something, else opens and asks again. Up to three tries, then pretends.
static bool fetch(void)
{
    for (int tries = 0; tries < 3; tries++) {
        s_turn_kind = K_FETCH;
        int a = await(E_TURN, ANSWER_S);
        if (a == A_NONE && !s_wrap) { say(SND_SC_FETCH_AGAIN); a = await(E_TURN, AGAIN_S); }
        if (a == A_STOP) return false;
        if (s_wrap) return true;
        if (a != A_TURN_OK) { say(SND_SC_FETCH_SKIP); return !gone(); }
        settle();
        if (pick_caught()) { say(SND_SC_FETCH_OK); return true; }
        say(SND_SC_FETCH_MISS);
        if (!job(OP_OPEN, 0)) return false;
    }
    say(SND_SC_FETCH_SKIP);
    return true;
}

// hand it back: the kid holds it and taps Let go (K_OPEN). Nobody takes it: put it down at the drop spot.
static bool give_back(void)
{
    if (!pick_holding()) return true;        // nothing borrowed (they skipped the fetch)
    s_turn_kind = K_GIVEBACK;
    int a = await(E_TURN, ANSWER_S);
    if (a == A_NONE && !s_wrap) { say(SND_SC_GIVEBACK_AGAIN); a = await(E_TURN, AGAIN_S); }
    if (a == A_STOP) return false;
    if (a == A_TURN_OK) { settle(); say(SND_SC_THANKS_LEND); return true; }
    say(SND_SC_PUT_DOWN);
    if (!job(OP_DROP, 0)) return false;
    return s_wrap || !gone();
}

static bool turn(int kind)
{
    if (kind == K_FETCH) return fetch();
    if (kind == K_GIVEBACK) return give_back();
    if (kind == K_STOP) {                    // "when I start dancing, shout stop!"
        uint32_t stops = s_stops;
        char msg[64];
        s_expect = E_STOP;
        pick_run(pick_holding() ? OP_BOW : OP_DANCE, 0, msg, sizeof msg);   // dancing opens the hand
        nap(150);
        for (int t = 0; t < 20000 && s_stops == stops && !pick_idle() && !s_wrap; t += 50) {
            if (t % 4000 == 0) voice_engage();
            nap(50);
        }
        s_expect = E_NONE;
        bool stopped = s_stops != stops;
        for (int quiet = 0; quiet < 2;) { nap(60); quiet = pick_idle() ? quiet + 1 : 0; }
        if (stopped) { s_silent = 0; say(SND_SC_TURN_OK); }
        else { say(SND_SC_TURN_SKIP); if (gone()) return false; }
        return true;
    }
    s_turn_kind = kind;
    int a = await(E_TURN, ANSWER_S);
    if (a == A_NONE && !s_wrap) { say(TAP(SND_SC_TURN_AGAIN)); a = await(E_TURN, AGAIN_S); }
    if (a == A_STOP) return false;
    if (a == A_TURN_OK) {
        settle();                            // the router queues the command right after letting it through
        say(SND_SC_TURN_OK);
        return true;
    }
    if (s_wrap) return true;
    say(SND_SC_TURN_SKIP);
    return !gone();
}

static void rank_check(int before)
{
    int r0 = rank_of(before), r1 = rank_of(P[s_kid].next);
    if (r1 == r0 || r1 >= SCHOOL_RANKS) return;
    say(SND_SC_RANK_UP);
    job(OP_CELEBRATE, 0);                    // a victory lap with bells
    say(SND_SC_RANK_0 + r1);
}

// teach lesson i: false if interrupted (it starts over next time)
static bool lesson_run(int i);
static bool lesson(int i) { bool ok = lesson_run(i); s_now_lesson = -1; return ok; }

static bool lesson_run(int i)
{
    bool replay = i < P[s_kid].next;         // one they've done before (Start lesson on the Learning page)
    s_now_lesson = i;
    if (!replay) say(SND_SC_NEW);
    int step = SCHOOL[i].step, end = SCHOOL[i].step + SCHOOL[i].nstep;
    while (step < end) {
        if (!job(OP_LESSON, step | end << 16)) return false;
        int nx = pick_lesson_next();
        if (nx <= step) return false;
        int kind = SCHOOL_STEP[nx - 1].turn;
        step = nx;
        if (s_wrap) return false;
        if (kind != K_NONE && !turn(kind)) return false;
    }
    job(OP_HOME, 0);
    if (!replay) {                           // a replay keeps its place in the course; its quiz stars still count
        int before = P[s_kid].next;
        lock();
        P[s_kid].next = (uint8_t)(i + 1);
        P[s_kid].lessons_today++;
        save();
        unlock();
        star();
        rank_check(before);
        if (P[s_kid].next >= SCHOOL_COUNT) { say(SND_SC_DONE_ALL); job(OP_CELEBRATE, 0); }
    }
    // check: two of its three questions
    say(SND_SC_CHECK);
    int a = (int)(esp_random() % SCHOOL_QPER), b = (a + 1 + (int)(esp_random() % (SCHOOL_QPER - 1))) % SCHOOL_QPER;
    if (question(SCHOOL[i].quiz + a) < 0) return false;
    if (question(SCHOOL[i].quiz + b) < 0) return false;
    return true;
}

static void free_play(void)
{
    if (s_content) say(TAP(SND_SC_FREE));
    s_heard_us = esp_timer_get_time();
    s_expect = E_FREE;
    while (!s_wrap && s_on && s_want_lesson < 0) {          // a Start lesson tap ends free play
        if (esp_timer_get_time() - s_heard_us > (int64_t)FREE_IDLE_S * 1000000 && pick_idle() && !fx_speaking()) {
            s_expect = E_NONE;
            say(TAP(SND_SC_AWAY));
            pause_class();
            return;
        }
        nap(100);
    }
    s_expect = E_NONE;
}

static void wrap(void)
{
    s_expect = E_NONE;
    for (int t = 0; t < 30000 && !pick_idle(); t += 50) nap(50);     // the lesson finishes its sentence
    for (int t = 0; t < 30000 && fx_speaking(); t += 50) nap(50);
    if (pick_holding()) { say(SND_SC_PUT_DOWN); job(OP_DROP, 0); }   // a borrowed thing: never sleep holding it
    job(OP_HOME, 0);
    if (s_content) {
        say(SND_SC_BELL);
        int st = P[s_kid].stars_today;
        if (st) say_n(SND_SC_STARS_1, st > SND_N_NUMBERS ? SND_N_NUMBERS : st);
        if (P[s_kid].next < SCHOOL_COUNT) say(SCHOOL[P[s_kid].next].teaser);
        say(SND_SC_NIGHT);
    } else say(SND_SC_OUT);
    job(OP_BOW, 0);
    lock(); save(); unlock();
    s_asleep_kid = s_kid;
    s_asleep_day = today();
    s_asleep_said = esp_timer_get_time();
    s_st = ST_ASLEEP;
    s_wrap = false;
    voice_hold_open(false);
    fx_sleep(true);
    ESP_LOGI(TAG, "school's out");
}

// a new day for kid k: reset the day's counters, carry the streak
static void open_day(int k)
{
    int32_t d = today();
    lock();
    if (P[k].day != d) {
        P[k].streak = streak_next(P[k].day, d, P[k].streak);
        P[k].day = d;
        P[k].used_s = P[k].extra_s = 0;
        P[k].lessons_today = P[k].stars_today = P[k].warmed = 0;
        save();
    }
    unlock();
}

static void session(void)
{
    int prev = s_kid;
    bool resume = s_paused && prev >= 0 && P[prev].day == today();   // paused earlier today: pick up again
    int k = s_want_kid >= 0 ? s_want_kid : resume ? prev : default_kid();
    s_kid = -1;                              // nobody's clock runs until we know whose class it is
    s_acc = 0;
    s_want_kid = -1;
    s_paused = false;
    s_wrap = false;
    s_silent = 0;
    s_st = ST_RUN;
    s_tick_us = esp_timer_get_time();
    fx_quiet(0);
    fx_sleep(false);
    pick_attention();
    voice_hold_open(true);                   // in class: no "Hey Muffin" needed
    s_content = fx_has(SCHOOL_STEP[0].g);
    job(OP_HOME, 0);

    if (resume && k == prev) say(SND_SC_BACK);
    else {
        s_said_tf_how = false;
        bool newbie = k >= 0 && P[k].next == 0 && P[k].stars == 0;
        say(newbie ? SND_SC_FIRST : SND_SC_HELLO);
        if (k < 0) {                         // several kids: who's here?
            say(SND_SC_WHO);
            int a = await(E_KID, ANSWER_S);
            if (a == A_NONE) { say(SND_SC_WHO); a = await(E_KID, AGAIN_S); }
            k = a == A_KID ? s_ans_kid : GUEST;
        }
        if (k != GUEST) kids_select(k);      // their jewel + jingle
    }
    lock();
    bool ok = has_time(k);
    unlock();
    if (!ok) { voice_hold_open(false); s_kid = k; s_st = ST_ASLEEP; s_asleep_kid = k; s_asleep_day = today(); fx_sleep(true); say(SND_SC_ASLEEP); return; }
    bool fresh = P[k].day != today();
    open_day(k);
    s_kid = k;
    s_tick_us = esp_timer_get_time();
    ESP_LOGI(TAG, "class for %s: %d s left, lesson %d", k == GUEST ? "guest" : kids_get(k)->name, left_s(k), P[k].next + 1);

    if (!resume) {
        if (fresh && P[k].streak >= 2) {
            if (P[k].streak > SND_N_NUMBERS) say(SND_SC_STREAK_BIG); else say_n(SND_SC_STREAK_1, P[k].streak);
            if (P[k].streak == 3 || P[k].streak % 5 == 0) job(OP_CELEBRATE, 0);   // streak milestones get a victory lap
        }
        int m = left_s(k) / 60;
        if (left_s(k) < 150) say(SND_SC_LAST);
        else if (m >= 2 && m < s_minutes && m <= SND_N_NUMBERS) say_n(SND_SC_MINUTES_1, m);
    }
    if (!s_content) { fx_say(SND_SC_NOCARD); free_play(); if (s_wrap) wrap(); return; }

    if (s_want_lesson >= 0) { }              // they picked a lesson: straight to it (below), no warm-up
    else if (P[k].next >= SCHOOL_COUNT) {    // finished the course: review, then play
        if (!P[k].warmed) { say(SND_SC_REVIEW); if (!warmup(3)) goto out; }
    } else {
        if (!P[k].warmed && P[k].next > 0) { say(SND_SC_WARMUP); if (!warmup(2)) goto out; }
        // (a lesson whose voice isn't on the card yet ends the lessons for now: SCHOOL_VOICED in make_sounds.py)
        while (!s_wrap && P[k].next < SCHOOL_COUNT && P[k].lessons_today < MAX_LESSONS && left_s(k) >= LESSON_MIN_S &&
               fx_has(SCHOOL_STEP[SCHOOL[P[k].next].step].g)) {
            if (!lesson(P[k].next)) goto out;
            if (s_wrap || P[k].next >= SCHOOL_COUNT || P[k].lessons_today >= MAX_LESSONS || left_s(k) < MORE_MIN_S ||
                !fx_has(SCHOOL_STEP[SCHOOL[P[k].next].step].g)) break;
            say(TAP(SND_SC_MORE));
            int a = await(E_YN, ANSWER_S);
            if (a == A_STOP) goto out;
            if (a != A_YES) break;
        }
    }
    while (!s_wrap) {                        // free play; a Start lesson tap runs that lesson, then back to play
        int L = s_want_lesson;
        s_want_lesson = -1;
        if (L >= 0 && L <= P[k].next && L < SCHOOL_COUNT && fx_has(SCHOOL_STEP[SCHOOL[L].step].g) && !lesson(L)) goto out;
        if (s_wrap) break;
        free_play();
        if (s_want_lesson < 0) break;        // walked away, or time's up
    }
out:
    s_want_lesson = -1;
    if (s_wrap) wrap();
    else if (s_st == ST_RUN) pause_class();  // interrupted by "stop" or silence
}

static int s_invites;
static int64_t s_invite_us, s_open_until;

static bool class_left(void)                 // can a class start now? (school on, time left today)
{
    if (!s_on || s_st != ST_IDLE) return false;
    int k = default_kid();
    if (k < 0) return true;                  // several kids: whoever answers
    lock(); bool ok = has_time(k); unlock();
    return ok;
}

// Muffin wants company: on power-on it wakes, waves and asks the kid to start; between classes it asks again
// now and then. Any answer while its ears are open starts class (school_filter, ST_IDLE). No clock runs.
static void reach_out(bool power_on)
{
    s_invite_us = esp_timer_get_time();
    pick_attention();
    job(OP_WAVE, 0);
    if (class_left()) {
        say(power_on ? TAP(SND_SC_WAKE) : TAP(SND_SC_INVITE));
        voice_hold_open(true);
        s_open_until = esp_timer_get_time() + OPEN_US;
    } else if (power_on && s_st != ST_ASLEEP) say(SND_GREET);
}

static void task(void *arg)
{
    int64_t day_check = 0;
    bool woke = false;
    for (;;) {
        nap(100);
        if (s_start && s_on) { s_start = false; s_open_until = 0; s_invites = 0; session(); s_invite_us = esp_timer_get_time(); continue; }
        s_start = false;
        int64_t now = esp_timer_get_time();
        if (!woke && now > WAKE_US) { woke = true; reach_out(true); continue; }
        if (s_open_until && now > s_open_until) { s_open_until = 0; if (s_st == ST_IDLE) voice_hold_open(false); }
        if (s_heard_us > s_invite_us) { s_invites = 0; s_invite_us = s_heard_us; }   // someone talked: start over
        if (woke && !s_open_until && s_invites < INVITE_MAX && now - s_invite_us > INVITE_US && class_left() &&
            arm_manual_us() < now - INVITE_US) { s_invites++; reach_out(false); continue; }
        if (s_st == ST_ASLEEP && now - day_check > 30000000) {          // a new day wakes it up
            day_check = now;
            lock();
            bool wake = !s_on || today() != s_asleep_day || has_time(s_asleep_kid);
            unlock();
            if (wake) { s_st = ST_IDLE; s_paused = false; fx_sleep(false); ESP_LOGI(TAG, "awake"); }
        }
    }
}

void school_init(i2c_master_bus_handle_t bus)
{
    s_mx = xSemaphoreCreateMutex();
    setenv("TZ", CONFIG_XARM_TZ, 1);
    tzset();
    load();
    nvs_handle_t h;                          // boot count: the "day" when there's no clock at all
    uint32_t boots = 0;
    if (nvs_open("xarm", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "boots", &boots);
        nvs_set_u32(h, "boots", ++boots);
        nvs_commit(h);
        nvs_close(h);
    }
    s_boot_day = -(int32_t)(boots & 0x3FFFFFFF);
    i2c_device_config_t d = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x51, .scl_speed_hz = 100000 };
    if (i2c_master_bus_add_device(bus, &d, &s_rtc) != ESP_OK) s_rtc = NULL;
    rtc_read();
    xTaskCreate(task, "school", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "%s, %d min a day, clock: %s", s_on ? "on" : "off", s_minutes, s_clock_src);
}

// ---------------------------------------------------------------- voice side
static bool has(const char *s, const char *w) { return strstr(s, w) != NULL; }
static bool said_true(const char *t)
{
    return has(t, "true") || has(t, "correct") || has(t, "right") || !strcmp(t, "yes") || has(t, "yes please") || !strcmp(t, "yeah");
}
static bool said_false(const char *t)
{
    return has(t, "false") || has(t, "wrong") || has(t, "not true") || has(t, "not right") || has(t, "not correct") ||
           !strcmp(t, "no") || has(t, "no thanks") || !strcmp(t, "nope");
}
static bool said_yes(const char *t)
{
    return !strcmp(t, "yes") || has(t, "yes please") || has(t, "sure") || !strcmp(t, "yeah") || has(t, "another") ||
           has(t, "one more");
}
static bool said_no(const char *t) { return !strcmp(t, "no") || has(t, "no thanks") || !strcmp(t, "nope"); }

static void grant(int k)                     // grown-up: ten more minutes
{
    if (k < 0 || k > KIDS_MAX) k = s_asleep_kid >= 0 ? s_asleep_kid : default_kid() >= 0 ? default_kid() : GUEST;
    lock();
    if (P[k].day != today()) { P[k].streak = streak_next(P[k].day, today(), P[k].streak); P[k].day = today();
                               P[k].used_s = P[k].extra_s = 0; P[k].lessons_today = P[k].stars_today = P[k].warmed = 0; }
    int cap = 64000 - s_minutes * 60;        // used_s counts to 65000: the limit has to stay below that
    P[k].extra_s = (uint16_t)(P[k].extra_s + EXTRA_S > cap ? cap : P[k].extra_s + EXTRA_S);
    save();
    unlock();
    s_wrap = false;
    if (s_st == ST_ASLEEP) { s_st = ST_IDLE; s_paused = false; fx_sleep(false); }
}

bool school_filter(const char *t, char *msg, size_t n)
{
    if (!s_on || !s_mx) return false;
    s_heard_us = esp_timer_get_time();
    if (s_pass[0] && pass_heard(t, s_pass)) {
        grant(s_st == ST_ASLEEP ? s_asleep_kid : s_kid);
        fx_say(SND_SC_GROWNUP);
        snprintf(msg, n, "grown-up: +10 min");
        return true;
    }
    int named = (has(t, "i'm ") || has(t, "i am ") || has(t, "it's ") || has(t, "my name")) ? kids_find(t) : -1;
    switch (s_st) {
    case ST_ASLEEP: {
        if (named >= 0) {
            lock(); bool ok = has_time(named); unlock();
            if (ok) { s_want_kid = named; s_paused = false; s_st = ST_IDLE; s_start = true; snprintf(msg, n, "class for %s", kids_get(named)->name); return true; }
        }
        int64_t now = esp_timer_get_time();
        if (now - s_asleep_said > ASLEEP_SAY_US) { s_asleep_said = now; fx_say(SND_SC_ASLEEP); }
        snprintf(msg, n, "asleep: school's over for today");
        return true;
    }
    case ST_IDLE:
        if (named >= 0) s_want_kid = named;
        s_start = true;
        snprintf(msg, n, s_paused ? "back to class" : "school's in");
        return true;
    case ST_RUN:
        break;
    }
    // in class
    switch (s_expect) {
    case E_TF:
        if (said_true(t) || said_false(t)) {     // "false" and "not true" win over the "true" inside them
            s_ans = said_false(t) ? A_FALSE : A_TRUE;
            snprintf(msg, n, "answer: %s", s_ans == A_TRUE ? "true" : "false");
            return true;
        }
        break;
    case E_YN:
        if (said_yes(t) || said_no(t)) { s_ans = said_yes(t) ? A_YES : A_NO; snprintf(msg, n, "%s", said_yes(t) ? "yes" : "no"); return true; }
        break;
    case E_KID: {
        int k = kids_find(t);
        if (k >= 0) { s_ans_kid = k; s_ans = A_KID; snprintf(msg, n, "hi %s", kids_get(k)->name); return true; }
        break;
    }
    case E_FREE:
        if (named >= 0) { snprintf(msg, n, "already in class"); return true; }   // no switching kids mid-class
        return false;                                                            // free play: anything goes
    }
    if (named >= 0 || has(t, "teach me") || has(t, "lesson") || has(t, "tell me more") || has(t, "quiz") ||
        has(t, "quiet") || has(t, "surprise") || has(t, "trick") || has(t, "true") || has(t, "false") ||
        said_yes(t) || said_no(t)) {
        static int64_t said;
        int64_t now = esp_timer_get_time();
        if (s_expect == E_NONE || now - said > 8000000) { said = now; if (s_expect != E_NONE) fx_say(SND_SC_ALREADY); }
        snprintf(msg, n, "in class");
        return true;
    }
    return false;
}

bool school_allow(int kind, char *msg, size_t n)
{
    if (!s_on || s_st != ST_RUN || s_expect == E_FREE) return true;
    if (s_expect == E_TURN) {
        if (kind_fits(s_turn_kind, kind)) { s_ans = A_TURN_OK; return true; }
        fx_say(SND_SC_TURN_WRONG);
        snprintf(msg, n, "your turn: a different command");
        return false;
    }
    static int64_t said;
    int64_t now = esp_timer_get_time();
    if (now - said > 8000000 && s_expect != E_NONE) { said = now; fx_say(SND_SC_CLASS); }
    snprintf(msg, n, "class first");
    return false;
}

void school_note_stop(void)
{
    s_stops++;
    if (s_expect == E_STOP) s_ans = A_STOP;
}

bool school_explain(int i, char *msg, size_t n)
{
    if (i < 0 || i >= SCHOOL_COUNT) return false;
    if (s_on && s_st != ST_RUN) { snprintf(msg, n, "not in class"); return false; }   // asleep or waiting: the limit holds
    if (!school_allow(K_PLAY, msg, n)) return false;            // not in the middle of a lesson or a question
    if (!fx_has(SCHOOL_STEP[SCHOOL[i].step].g)) { fx_say(SND_SC_NOCARD); snprintf(msg, n, "lessons are on the card"); return false; }
    fx_say(SND_EXPLAIN);
    int first = SCHOOL[i].step + 1, end = SCHOOL[i].step + SCHOOL[i].nstep;   // skips "Lesson five."
    return pick_run(OP_LESSON, first | end << 16 | LESSON_EXPLAIN, msg, n);
}

bool school_wrap_due(void) { return s_wrap; }
bool school_asleep(void) { return s_on && s_st == ST_ASLEEP; }
bool school_chatter_ok(void) { return !s_on || s_st == ST_IDLE; }
bool school_in_lesson(void) { return s_on && s_st == ST_RUN && s_expect != E_FREE; }

// ---------------------------------------------------------------- web
// what Muffin is waiting for right now, as buttons for the Learning page's pop-up: each `say` goes through the
// router as if the kid said it (POST /api/cmd {text, voice:true}), so it counts exactly like speaking.
static void opt(cJSON *a, const char *label, const char *say)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "label", label);
    cJSON_AddStringToObject(o, "say", say);
    cJSON_AddItemToArray(a, o);
}

static void turn_opts(cJSON *a, int k)
{
    switch (k) {
    case K_UP: opt(a, "A little up", "a little up"); break;
    case K_DOWN: opt(a, "A little down", "a little down"); break;
    case K_LEFT: opt(a, "A little left", "a little left"); break;
    case K_RIGHT: opt(a, "A little right", "a little right"); break;
    case K_TURN: opt(a, "Turn left", "turn left"); opt(a, "Turn right", "turn right"); break;
    case K_TILT: opt(a, "Tilt up", "tilt up"); opt(a, "Tilt down", "tilt down"); break;
    case K_OPEN: opt(a, "Open your hand", "open your hand"); break;
    case K_CLOSE: opt(a, "Close your hand", "close your hand"); break;
    case K_BELL: opt(a, "Ring the bell", "ring the bell"); break;
    case K_DANCE: opt(a, "Dance!", "lets dance"); break;
    case K_WAVE: opt(a, "Wave hello", "wave hello"); break;
    case K_BOW: opt(a, "Take a bow", "take a bow"); break;
    case K_HOME: opt(a, "Go home", "go home"); break;
    case K_FETCH: opt(a, "Clamp!", "grab it"); break;
    case K_GIVEBACK: opt(a, "Let go", "let go"); break;
    default: opt(a, "Up", "a little up"); opt(a, "Down", "a little down");
             opt(a, "Left", "a little left"); opt(a, "Right", "a little right"); break;
    }
}

static void ask_json(cJSON *o)
{
    int e = s_expect;
    if (s_tap && class_left()) {            // taps only: the pop-up is how class starts
        cJSON *ask = cJSON_AddObjectToObject(o, "ask");
        cJSON_AddStringToObject(ask, "kind", s_paused ? "resume" : "start");
        opt(cJSON_AddArrayToObject(ask, "options"), s_paused ? "Keep going" : "Start class", "lets go");
        return;
    }
    if (s_tap && s_st == ST_RUN && e == E_FREE) {   // free play: the tricks
        cJSON *ask = cJSON_AddObjectToObject(o, "ask");
        cJSON_AddStringToObject(ask, "kind", "free");
        cJSON *a = cJSON_AddArrayToObject(ask, "options");
        opt(a, "Dance!", "lets dance"); opt(a, "Wave hello", "wave hello"); opt(a, "Ring the bell", "ring the bell");
        opt(a, "Take a bow", "take a bow"); opt(a, "Open hand", "open your hand"); opt(a, "Close hand", "close your hand");
        return;
    }
    if (s_st != ST_RUN || e == E_NONE || e == E_FREE) return;
    cJSON *ask = cJSON_AddObjectToObject(o, "ask");
    cJSON *a = cJSON_AddArrayToObject(ask, "options");
    switch (e) {
    case E_TF:
        cJSON_AddStringToObject(ask, "kind", "tf");
        if (s_q >= 0) cJSON_AddNumberToObject(ask, "q", s_q);
        opt(a, "True", "true"); opt(a, "False", "false");
        break;
    case E_YN:
        cJSON_AddStringToObject(ask, "kind", "yn");
        opt(a, "Yes please", "yes please"); opt(a, "No thanks", "no thanks");
        break;
    case E_KID:
        cJSON_AddStringToObject(ask, "kind", "kid");
        for (int i = 0; i < KIDS_MAX; i++) if (kids_get(i)) {
            char say[48]; snprintf(say, sizeof say, "hi i'm %s", kids_get(i)->name);
            opt(a, kids_get(i)->name, say);
        }
        break;
    case E_STOP:
        cJSON_AddStringToObject(ask, "kind", "stop");
        opt(a, "Stop!", "stop");
        break;
    default:
        cJSON_AddStringToObject(ask, "kind", s_turn_kind == K_FETCH ? "fetch" : s_turn_kind == K_GIVEBACK ? "giveback" : "turn");
        turn_opts(a, s_turn_kind);
        break;
    }
}

static const char *state_name(void)
{
    if (!s_on) return "off";
    return s_st == ST_RUN ? "in class" : s_st == ST_ASLEEP ? "asleep" : s_paused ? "paused" : "waiting";
}

static void kid_json(cJSON *arr, int k, const char *name)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "i", k);
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddNumberToObject(o, "lesson", P[k].next);                      // lessons finished
    cJSON_AddNumberToObject(o, "stars", P[k].stars);
    cJSON_AddNumberToObject(o, "streak", P[k].streak);
    bool td = P[k].day == today();
    cJSON_AddNumberToObject(o, "used_s", td ? P[k].used_s : 0);
    cJSON_AddNumberToObject(o, "limit_s", td ? limit_s(k) : s_minutes * 60);
    cJSON_AddItemToArray(arr, o);
}

void school_json(cJSON *o)
{
    lock();
    cJSON_AddBoolToObject(o, "on", s_on);
    cJSON_AddNumberToObject(o, "minutes", s_minutes);
    cJSON_AddBoolToObject(o, "pass_set", s_pass[0] != 0);
    cJSON_AddStringToObject(o, "state", state_name());
    cJSON_AddStringToObject(o, "kid", s_kid == GUEST ? "guest" : s_kid >= 0 && kids_get(s_kid) ? kids_get(s_kid)->name : "");
    cJSON_AddStringToObject(o, "clock", s_clock_ok ? s_clock_src : "none");
    cJSON_AddBoolToObject(o, "lessons_ready", fx_has(SCHOOL_STEP[0].g));
    cJSON_AddNumberToObject(o, "lesson_count", SCHOOL_COUNT);
    cJSON_AddBoolToObject(o, "tap", s_tap);
    cJSON *arr = cJSON_AddArrayToObject(o, "kids");
    for (int k = 0; k < KIDS_MAX; k++) if (kids_get(k)) kid_json(arr, k, kids_get(k)->name);
    kid_json(arr, GUEST, "guest");
    unlock();
    ask_json(o);                             // after unlock: it takes the lock itself
    cJSON_AddNumberToObject(o, "now", s_st == ST_RUN ? s_now_lesson : -1);   // lesson being taught, -1 = none
    cJSON_AddBoolToObject(o, "can_pick", class_left() || (s_st == ST_RUN && s_expect == E_FREE));   // Start lesson buttons
}

// Start lesson (Learning page): lesson i if the kid has reached it (done, or the next one). Starts class when it's
// waiting; in free play it ends the play and runs that lesson. Not mid-lesson.
static bool pick_lesson(int i, char *msg, size_t n)
{
    int k = s_st == ST_RUN ? s_kid : default_kid();
    if (k < 0) k = GUEST;
    if (i < 0 || i >= SCHOOL_COUNT || i > P[k].next) { snprintf(msg, n, "that lesson isn't open yet"); return false; }
    if (!fx_has(SCHOOL_STEP[SCHOOL[i].step].g)) { snprintf(msg, n, "that lesson isn't on the card yet"); return false; }
    if (s_st == ST_RUN && s_expect == E_FREE) { s_want_lesson = i; s_heard_us = esp_timer_get_time(); snprintf(msg, n, "lesson %d", i + 1); return true; }
    if (!class_left()) { snprintf(msg, n, s_st == ST_ASLEEP ? "Muffin is asleep" : "finish this part first"); return false; }
    s_want_lesson = i;
    s_start = true;
    snprintf(msg, n, "lesson %d", i + 1);
    return true;
}

// {"on":bool, "tap":bool, "lesson":i, "minutes":5..120, "pass":"...", "more":kid, "reset_day":kid, "restart":kid}
bool school_set(const cJSON *b, char *msg, size_t n)
{
    const cJSON *v;
    bool changed = false, pass_changed = false;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(b, "on"))) {
        s_on = cJSON_IsTrue(v); changed = true;
        if (!s_on) { s_wrap = false; voice_hold_open(false); if (s_st == ST_ASLEEP) { s_st = ST_IDLE; fx_sleep(false); } }
    }
    if (cJSON_IsBool(v = cJSON_GetObjectItem(b, "tap"))) { s_tap = cJSON_IsTrue(v); changed = true; voice_hold_open(false); }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(b, "minutes"))) {
        int m = v->valueint < 5 ? 5 : v->valueint > 120 ? 120 : v->valueint;
        s_minutes = (uint8_t)m; changed = true;
    }
    if (cJSON_IsString(v = cJSON_GetObjectItem(b, "pass"))) {
        char w[48] = "";
        size_t k = 0;
        for (const char *c = v->valuestring; *c && k < sizeof w - 1; c++)   // letters and spaces: it's a voice phrase
            if ((*c >= 'a' && *c <= 'z') || *c == ' ') w[k++] = *c;
            else if (*c >= 'A' && *c <= 'Z') w[k++] = (char)(*c - 'A' + 'a');
        w[k] = 0;
        if (w[0] && !pass_heard(w, w)) { snprintf(msg, n, "passphrase needs at least 6 letters"); return false; }
        strlcpy(s_pass, w, sizeof s_pass); changed = pass_changed = true;
    }
    if (changed) save_settings();
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(b, "lesson"))) return pick_lesson(v->valueint, msg, n);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(b, "more"))) grant(v->valueint);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(b, "reset_day")) && v->valueint >= 0 && v->valueint <= KIDS_MAX) {
        lock(); P[v->valueint].used_s = P[v->valueint].extra_s = 0; P[v->valueint].lessons_today = 0; save(); unlock();
        if (s_st == ST_ASLEEP && s_asleep_kid == v->valueint) { s_st = ST_IDLE; fx_sleep(false); }
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(b, "restart")) && v->valueint >= 0 && v->valueint <= KIDS_MAX)
        school_kid_reset(v->valueint);
    if (pass_changed) voice_reload();        // MultiNet learns the passphrase too
    snprintf(msg, n, "ok");
    return true;
}

bool school_tap(void) { return s_on && s_tap; }   // School off: ears work as usual
