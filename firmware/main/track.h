// track.h — follows the LD2450's targets from frame to frame and says what each one moves like.
//
//   person    walked: it moved at least TRK_WALK_CM from where it first showed up (sticky for that track)
//   swinging  back and forth in place, regularly: same rhythm and the same reach every swing (a cable, a fan,
//             a curtain). Doesn't count as someone at the bench.
//   new       anything else, including a person who was already sitting there. Counts as someone, like before.
//
// Only a clear, regular swing is left out, so a still person is never mistaken for clutter. Pure C, no ESP-IDF:
// host test in firmware/test/test_track.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TRK_MAX 3
#define TRK_WALK_CM 60          // moved this far from where it appeared: a person walked in
#define TRK_JOIN_CM 60          // a new point joins the track whose last point is this close
#define TRK_LOST_MS 1500        // a track nobody's matched for this long is gone
#define TRK_HYST_CM 8           // a turn-around only counts after backing off this far (radar jitter is smaller)
#define TRK_REVS 8              // turn-arounds needed (7 swings) before calling it swinging

typedef enum { TRK_NEW, TRK_PERSON, TRK_SWING } trk_kind;

typedef struct {                // one axis (x or y) of a track: where it turned around, and when
    int dir;                    // +1 / -1 heading, 0 not yet
    float ext;                  // the furthest point in the current heading
    int64_t t[TRK_REVS];        // the last TRK_REVS turn-arounds (oldest first)
    float at[TRK_REVS];
    int n;
} trk_axis;

typedef struct {
    bool on;
    int id;                     // counts up, so the panel can tell tracks apart
    int64_t born_ms, seen_ms;
    float x, y, v;              // newest point (cm, cm, cm/s)
    float x0, y0;               // where it first showed up
    float walked;               // the furthest it's been from there
    float bx0, bx1, by0, by1;   // everywhere it's been (a ready-made ignore zone for a swinging thing)
    trk_axis ax, ay;
    trk_kind kind;
} trk_t;

// one radar frame: n points {x_cm, y_cm, speed_cm_s}. Updates tr[] in place.
void trk_update(trk_t tr[TRK_MAX], const int16_t pts[][3], int n, int64_t now_ms);
const char *trk_name(trk_kind k);
