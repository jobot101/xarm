#include "router.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "moves.h"
#include "pick.h"
#include "store.h"

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
    if (has(t, "stop") || has(t, "freeze"))                         { pick_stop(); snprintf(msg, n, "stopped"); return true; }
    if (has(t, "give") || has(t, "hand it"))                        return pick_run(OP_GIVE, 0, msg, n);
    if (has(t, "sort") || has(t, "clean up") || has(t, "tidy"))     return pick_run(OP_SORT, 0, msg, n);
    if (has(t, "wave") || has(t, "hello") || has(t, "say hi"))      return pick_run(OP_WAVE, 0, msg, n);
    if (has(t, "bow"))                                              return pick_run(OP_BOW, 0, msg, n);
    if (has(t, "dance") || has(t, "boogie"))                        return pick_run(OP_DANCE, 0, msg, n);
    if (has(t, "bell") || has(t, "ring"))                           return pick_run(OP_BELL, 0, msg, n);
    if (has(t, "home"))                                             return pick_run(OP_HOME, 0, msg, n);
    if (has(t, "drop") || has(t, "put it down") || has(t, "let go")) return pick_run(OP_DROP, 0, msg, n);
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
    ADD("wave hello");
    ADD("take a bow");
    ADD("dance");
    ADD("ring the bell");
    ADD("open hand");
    ADD("close hand");
    for (int i = 0; i < NSLOT; i++) {
        if (!g_cfg.slot_name[i][0]) continue;
        ADD("pick up the %s", g_cfg.slot_name[i]);
        ADD("grab the %s", g_cfg.slot_name[i]);
    }
    for (int i = 0; i < MOVE_MAX; i++) if (moves_name(i)[0]) ADD("do the %s", moves_name(i));
#undef ADD
    return n;
}
