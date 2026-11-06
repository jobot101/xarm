// track.c — see track.h.
#include "track.h"
#include <math.h>
#include <string.h>

const char *trk_name(trk_kind k) { return k == TRK_PERSON ? "person" : k == TRK_SWING ? "swinging" : "new"; }

static void axis_feed(trk_axis *a, float v, int64_t now)
{
    if (!a->dir) {                                   // first heading: once it's backed off far enough from the start
        if (fabsf(v - a->ext) >= TRK_HYST_CM) a->dir = v > a->ext ? 1 : -1;
        else return;
    }
    if ((a->dir > 0 && v > a->ext) || (a->dir < 0 && v < a->ext)) { a->ext = v; return; }
    if (fabsf(v - a->ext) < TRK_HYST_CM) return;
    if (a->n == TRK_REVS) { memmove(a->t, a->t + 1, sizeof a->t[0] * (TRK_REVS - 1)); memmove(a->at, a->at + 1, sizeof a->at[0] * (TRK_REVS - 1)); a->n--; }
    a->t[a->n] = now; a->at[a->n] = a->ext; a->n++;   // turned around at ext
    a->dir = -a->dir; a->ext = v;
}

static float cv(const float *x, int n, float *mean)  // spread / mean
{
    float m = 0, s = 0;
    for (int i = 0; i < n; i++) m += x[i];
    m /= n;
    for (int i = 0; i < n; i++) s += (x[i] - m) * (x[i] - m);
    *mean = m;
    return m > 0 ? sqrtf(s / n) / m : 99;
}

// a regular swing on this axis: TRK_REVS turn-arounds, evenly spaced in time and reaching the same distance,
// and still going (the last one wasn't long ago)
static bool axis_swings(const trk_axis *a, int64_t now)
{
    if (a->n < TRK_REVS) return false;
    float dt[TRK_REVS - 1], amp[TRK_REVS - 1], mt, ma;
    for (int i = 1; i < TRK_REVS; i++) {
        dt[i - 1] = (float)(a->t[i] - a->t[i - 1]);
        amp[i - 1] = fabsf(a->at[i] - a->at[i - 1]);
        if (dt[i - 1] < 250 || dt[i - 1] > 12000) return false;     // faster than a radar sees, or slower than a fan
    }
    float ct = cv(dt, TRK_REVS - 1, &mt), ca = cv(amp, TRK_REVS - 1, &ma);
    if ((float)(now - a->t[TRK_REVS - 1]) > fmaxf(2.5f * mt, 2000)) return false;   // it stopped swinging
    return ct < 0.15f && ca < 0.2f && ma >= 12;        // a pendulum or fan repeats within ~10%; fidgeting doesn't
}

static void start(trk_t *t, const int16_t *p, int64_t now)
{
    static int next_id = 1;
    memset(t, 0, sizeof *t);
    t->on = true; t->id = next_id++;
    t->born_ms = t->seen_ms = now;
    t->x = t->x0 = t->bx0 = t->bx1 = p[0];
    t->y = t->y0 = t->by0 = t->by1 = p[1];
    t->v = p[2];
    t->ax.ext = p[0]; t->ay.ext = p[1];
}

static void feed(trk_t *t, const int16_t *p, int64_t now)
{
    t->x = p[0]; t->y = p[1]; t->v = p[2]; t->seen_ms = now;
    t->bx0 = fminf(t->bx0, t->x); t->bx1 = fmaxf(t->bx1, t->x);
    t->by0 = fminf(t->by0, t->y); t->by1 = fmaxf(t->by1, t->y);
    t->walked = fmaxf(t->walked, hypotf(t->x - t->x0, t->y - t->y0));
    axis_feed(&t->ax, t->x, now);
    axis_feed(&t->ay, t->y, now);
    if (t->kind == TRK_PERSON) return;                       // once it walked, it's a person for as long as it's tracked
    if (t->walked >= TRK_WALK_CM) t->kind = TRK_PERSON;
    else t->kind = axis_swings(&t->ax, now) || axis_swings(&t->ay, now) ? TRK_SWING : TRK_NEW;
}

void trk_update(trk_t tr[TRK_MAX], const int16_t pts[][3], int n, int64_t now)
{
    bool used[TRK_MAX] = {0}, got[TRK_MAX] = {0};
    if (n > TRK_MAX) n = TRK_MAX;
    for (;;) {                                               // pair points with tracks, closest pair first
        int bt = -1, bp = -1;
        float bd = TRK_JOIN_CM;
        for (int t = 0; t < TRK_MAX; t++) {
            if (!tr[t].on || got[t]) continue;
            for (int p = 0; p < n; p++) {
                if (used[p]) continue;
                float d = hypotf(pts[p][0] - tr[t].x, pts[p][1] - tr[t].y);
                if (d < bd) { bd = d; bt = t; bp = p; }
            }
        }
        if (bt < 0) break;
        feed(&tr[bt], pts[bp], now);
        got[bt] = used[bp] = true;
    }
    for (int t = 0; t < TRK_MAX; t++)
        if (tr[t].on && !got[t] && now - tr[t].seen_ms > TRK_LOST_MS) tr[t].on = false;
    for (int p = 0; p < n; p++) {                            // the rest start tracks
        if (used[p]) continue;
        for (int t = 0; t < TRK_MAX; t++)
            if (!tr[t].on) { start(&tr[t], pts[p], now); break; }
    }
}
