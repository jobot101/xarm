#include "router.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "moves.h"
#include "pick.h"
#include "store.h"
#include "kids.h"

static bool starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
static bool has(const char *s, const char *w) { return strstr(s, w) != NULL; }

// "pick up the red one" -> slot whose name appears in the text
static int find_slot(const char *t)
{
    for (int i = 0; i < NSLOT; i++)
        if (g_cfg.slot_name[i][0] && strstr(t, g_cfg.slot_name[i])) return i;
    return -1;
}

bool router_exec(const char *text, char *msg, size_t n)
{
    char t[64];
    size_t k = 0;
    for (; text[k] && k < sizeof t - 1; k++) t[k] = (char)tolower((unsigned char)text[k]);
    t[k] = 0;

    // order matters: specific phrases before generic words
    if (has(t, "stop") || has(t, "freeze") || has(t, "halt") || has(t, "wait"))                         { pick_stop(); snprintf(msg, n, "stopped"); return true; }
    // third hand
    if (has(t, "hold this") || has(t, "help me") || has(t, "third hand"))  return pick_run(OP_HELPER, 0, msg, n);
    if (has(t, "hold it") || has(t, "grip") || has(t, "grab it") || has(t, "squeeze")) return pick_run(OP_CLOSE, 0, msg, n);
    if (has(t, "let go") || has(t, "let it go") || has(t, "release") || has(t, "drop the"))                      return pick_run(OP_OPEN, 0, msg, n);
    if (has(t, "little") || has(t, "nudge") || starts(t, "move") || has(t, "tilt") || has(t, "turn")) {
        int d = has(t, "tilt") ? (has(t, "down") ? NUDGE_TILT_DOWN : NUDGE_TILT_UP)
              : has(t, "turn") ? (has(t, "right") ? NUDGE_TURN_R : NUDGE_TURN_L)
              : has(t, "left") ? NUDGE_LEFT : has(t, "right") ? NUDGE_RIGHT
              : has(t, "up") || has(t, "higher") ? NUDGE_UP : has(t, "down") || has(t, "lower") ? NUDGE_DOWN : -1;
        if (d >= 0) return pick_run(OP_NUDGE, d, msg, n);
    }
    // stacking challenge
    if (has(t, "tower") || has(t, "stack"))                         return pick_run(OP_STACK, 0, msg, n);
    // who's playing: "hi i'm sam", "it's sam"
    if (has(t, "i'm ") || has(t, "i am ") || has(t, "it's ") || has(t, "my name")) {
        int k = kids_find(t);
        if (k >= 0) { kids_select(k); snprintf(msg, n, "hi %s!", kids_get(k)->name); return pick_run(OP_WAVE, 0, msg, n) || true; }
    }
    if (has(t, "give") || has(t, "hand it") || has(t, "bring it") || has(t, "come here"))                        return pick_run(OP_GIVE, 0, msg, n);
    if (has(t, "sort") && (has(t, "size") || has(t, "big") || has(t, "small"))) return pick_run(OP_SORT_SIZE, 0, msg, n);
    if (has(t, "sort") || has(t, "clean up") || has(t, "tidy"))     return pick_run(OP_SORT, 0, msg, n);
    if (has(t, "wave") || has(t, "hello") || has(t, "say hi") || has(t, "goodbye") || has(t, "bye"))      return pick_run(OP_WAVE, 0, msg, n);
    if (has(t, "bow") || has(t, "thank"))                                              return pick_run(OP_BOW, 0, msg, n);
    if (has(t, "dance") || has(t, "boogie") || has(t, "party"))                        return pick_run(OP_DANCE, 0, msg, n);
    if (has(t, "bell") || has(t, "ring"))                           return pick_run(OP_BELL, 0, msg, n);
    if (has(t, "home") || has(t, "go back") || has(t, "rest") || has(t, "relax"))                                             return pick_run(OP_HOME, 0, msg, n);
    if (has(t, "drop") || has(t, "put it down"))                    return pick_run(OP_DROP, 0, msg, n);
    if (has(t, "open"))                                             return pick_run(OP_OPEN, 0, msg, n);
    if (has(t, "close"))                                            return pick_run(OP_CLOSE, 0, msg, n);
    if (starts(t, "pick") || starts(t, "grab") || starts(t, "get") || starts(t, "fetch")) {
        int s = find_slot(t);
        if (s < 0) { snprintf(msg, n, "pick what? I know: %s, %s", g_cfg.slot_name[0], g_cfg.slot_name[1]); return false; }
        return pick_run(OP_PICK, s, msg, n);
    }
    int m = moves_find(t);                     // "do the robot", "play twirl", or just the name
    if (m >= 0) return pick_run(OP_PLAY, m, msg, n);
    snprintf(msg, n, "unknown command");
    return false;
}

int router_phrases(char out[][48], int max)
{
    int n = 0;
#define ADD(...) do { if (n < max) snprintf(out[n++], 48, __VA_ARGS__); } while (0)
    ADD("stop");
    ADD("go home");
    ADD("drop it");
    ADD("give it to me");
    ADD("sort everything");
    ADD("sort by size");
    ADD("wave hello");
    ADD("take a bow");
    ADD("dance");
    ADD("dance robot");
    ADD("robot dance");
    ADD("ring the bell");
    ADD("open hand");
    ADD("close hand");
    ADD("hold this");
    ADD("hold it");
    ADD("let go");
    ADD("a little left");
    ADD("a little right");
    ADD("a little up");
    ADD("a little down");
    ADD("build a tower");
    // more ways to say the same things (each must route through router_exec above)
    ADD("freeze");
    ADD("halt");
    ADD("wait");
    ADD("come home");
    ADD("go back");
    ADD("rest");
    ADD("put it down");
    ADD("hand it over");
    ADD("bring it here");
    ADD("come here");
    ADD("clean up");
    ADD("tidy up");
    ADD("hello");
    ADD("say hello");
    ADD("hello robot");
    ADD("goodbye");
    ADD("thank you");
    ADD("bow down");
    ADD("lets dance");
    ADD("party time");
    ADD("boogie");
    ADD("ring it");
    ADD("open");
    ADD("open up");
    ADD("close");
    ADD("grab it");
    ADD("squeeze");
    ADD("release");
    ADD("let it go");
    ADD("help me");
    ADD("third hand");
    ADD("stack them");
    ADD("move left");
    ADD("move right");
    ADD("move up");
    ADD("move down");
    ADD("a little higher");
    ADD("a little lower");
    ADD("turn left");
    ADD("turn right");
    ADD("tilt up");
    ADD("tilt down");
    for (int i = 0; i < KIDS_MAX; i++) if (kids_get(i)) ADD("hi i'm %s", kids_get(i)->name);
    for (int i = 0; i < NSLOT; i++) {
        if (!g_cfg.slot_name[i][0]) continue;
        ADD("pick up the %s", g_cfg.slot_name[i]);
        ADD("grab the %s", g_cfg.slot_name[i]);
        ADD("get the %s", g_cfg.slot_name[i]);
        ADD("fetch the %s", g_cfg.slot_name[i]);
    }
    for (int i = 0; i < MOVE_MAX; i++) if (moves_name(i)[0]) ADD("do the %s", moves_name(i));
#undef ADD
    return n;
}
