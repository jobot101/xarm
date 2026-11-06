// host test (CI runs it): cc -I../main test_track.c ../main/track.c -lm -o tt && ./tt
// Simulated LD2450 targets at 10 frames/s with radar jitter: what does each one get called?
#include "track.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned seed = 12345;
static float rnd(void) { seed = seed * 1103515245u + 12345u; return ((seed >> 8) & 0xFFFF) / 65535.0f * 2 - 1; }   // -1..1

static trk_t tr[TRK_MAX];
static int64_t now;

static void frame(int n, const float xy[][2])
{
    int16_t p[TRK_MAX][3];
    for (int i = 0; i < n; i++) { p[i][0] = (int16_t)lroundf(xy[i][0]); p[i][1] = (int16_t)lroundf(xy[i][1]); p[i][2] = 0; }
    trk_update(tr, (const int16_t (*)[3])p, n, now);
    now += 100;
}

static trk_t *only(void) { for (int i = 0; i < TRK_MAX; i++) if (tr[i].on) return &tr[i]; return NULL; }
static void reset(void) { memset(tr, 0, sizeof tr); now = 1000; }
static void ok(bool c, const char *what) { printf("%s %s\n", c ? "ok  " : "FAIL", what); assert(c); }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    float j = 4;                                                   // radar jitter, +/- cm per frame

    // a person walks in from 2.5 m to 80 cm in 3 s, then hovers (jitter + slow wander) for a minute
    reset();
    int swing = 0;
    for (int i = 0; i < 30; i++) frame(1, (float[][2]){{20 + rnd() * j, 250 - i * 170 / 30.0f + rnd() * j}});
    ok(only() && only()->kind == TRK_PERSON, "walked in: person");
    for (int i = 0; i < 600; i++) {
        frame(1, (float[][2]){{20 + 15 * sinf(i / 37.0f) + rnd() * j, 80 + 10 * sinf(i / 23.0f) + rnd() * j}});
        swing += only()->kind == TRK_SWING;
    }
    ok(only()->kind == TRK_PERSON && !swing, "then hovering at the bench: still a person");

    // someone already sitting there: never walked, fidgets irregularly (shifts, leans, bursts of jitter)
    reset(); swing = 0;
    float x = -30, y = 90;
    for (int i = 0; i < 3000; i++) {                               // 5 minutes
        if (rnd() > 0.9f) { x += rnd() * 12; y += rnd() * 12; }   // a shift now and then
        x += (-30 - x) * 0.02f; y += (90 - y) * 0.02f;            // drifting back to the chair
        float jj = rnd() > 0.95f ? 10 : j;
        frame(1, (float[][2]){{x + rnd() * jj, y + rnd() * jj}});
        swing += only()->kind == TRK_SWING;
    }
    ok(!swing, "sitting and fidgeting for 5 min: never called swinging");
    ok(only()->kind == TRK_NEW, "  (and it still counts as someone)");

    // a hanging cable swinging +/-20 cm, 1.6 s a swing, slowly dying down
    reset();
    int64_t t_swing = -1;
    for (int i = 0; i < 300; i++) {
        float a = 20 * expf(-i / 600.0f);
        frame(1, (float[][2]){{40 + a * sinf(i * 2 * 3.14159f / 16) + rnd() * j, 100 + rnd() * j}});
        if (t_swing < 0 && only()->kind == TRK_SWING) t_swing = now - 1000;
    }
    ok(t_swing >= 0 && t_swing <= 10000, "swinging cable: swinging within 10 s");
    printf("     (after %.1f s)\n", t_swing / 1000.0);
    ok(only()->kind == TRK_SWING, "  and stays swinging");
    for (int i = 0; i < 60; i++) frame(1, (float[][2]){{40 + rnd() * j, 100 + rnd() * j}});   // it stops
    ok(only()->kind == TRK_NEW, "cable stops: back to counting (no longer swinging)");

    // a fan's head sweeping +/-30 cm across, 12 s a sweep, diagonal
    reset(); t_swing = -1;
    for (int i = 0; i < 1200; i++) {
        float s = sinf(i * 2 * 3.14159f / 120);
        frame(1, (float[][2]){{-60 + 30 * s + rnd() * j, 150 + 15 * s + rnd() * j}});
        if (t_swing < 0 && only()->kind == TRK_SWING) t_swing = now - 1000;
    }
    ok(t_swing >= 0 && only()->kind == TRK_SWING, "oscillating fan: swinging");
    printf("     (after %.1f s)\n", t_swing / 1000.0);

    // a person walks past the swinging cable: two tracks, each keeps its own label
    reset();
    for (int i = 0; i < 100; i++) frame(1, (float[][2]){{40 + 20 * sinf(i * 2 * 3.14159f / 16) + rnd() * j, 100 + rnd() * j}});
    for (int i = 0; i < 40; i++)
        frame(2, (float[][2]){{40 + 20 * sinf((100 + i) * 2 * 3.14159f / 16) + rnd() * j, 100 + rnd() * j},
                              {-150 + i * 6 + rnd() * j, 220 - i * 2 + rnd() * j}});
    int ns = 0, np = 0;
    for (int i = 0; i < TRK_MAX; i++) if (tr[i].on) { ns += tr[i].kind == TRK_SWING; np += tr[i].kind == TRK_PERSON; }
    ok(ns == 1 && np == 1, "person walks past the cable: one swinging, one person");

    // a target that vanishes is dropped after TRK_LOST_MS
    for (int i = 0; i < 20; i++) frame(0, NULL);
    ok(!only(), "gone for 2 s: no tracks left");

    printf("all track tests passed\n");
    return 0;
}
