#include "router.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "moves.h"
#include "pick.h"
#include "store.h"
#include "kids.h"
#include "chat.h"
#include "school.h"
#include "cam.h"
#include "fx.h"

static bool starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
static bool has(const char *s, const char *w) { return strstr(s, w) != NULL; }

// "pick up the red one" -> slot whose name appears in the text
static int find_slot(const char *t)
{
    for (int i = 0; i < NSLOT; i++)
        if (g_cfg.slot_name[i][0] && strstr(t, g_cfg.slot_name[i])) return i;
    return -1;
}

// Robot School gets a say on every voice command (school.c); the app (a grown-up) is never limited.
// RUN(kind, call): run the action unless School says it's not the moment.
#define RUN(kind, call) ((app || school_allow((kind), msg, n)) ? (call) : false)

bool router_exec(const char *text, char *msg, size_t n) { return router_exec_ex(text, msg, n, false); }

bool router_exec_ex(const char *text, char *msg, size_t n, bool app)
{
    char t[64];
    size_t k = 0;
    for (; text[k] && k < sizeof t - 1; k++) t[k] = (char)tolower((unsigned char)text[k]);
    t[k] = 0;

    // order matters: specific phrases before generic words
    if (has(t, "stop") || has(t, "freeze") || has(t, "halt") || has(t, "wait"))                         { pick_stop(); chat_stopped(); school_note_stop(); snprintf(msg, n, "stopped"); return true; }
    // class, sleep, grown-up passphrase, quiz answers
    if (!app && school_filter(t, msg, n)) return true;
    // Muffin's conversation (name, small talk, jokes, yes/no answers...) — before the tricks, which match loosely
    if ((app || !school_in_lesson()) && chat_exec(t, msg, n)) return true;
    // third hand
    if (has(t, "hold this") || has(t, "help me") || has(t, "third hand"))  return RUN(K_HOLD, pick_run(OP_HELPER, 0, msg, n));
    if (has(t, "hold it") || has(t, "grip") || has(t, "grab it") || has(t, "squeeze")) return RUN(K_CLOSE, pick_run(OP_CLOSE, 0, msg, n));
    if (has(t, "let go") || has(t, "let it go") || has(t, "release") || has(t, "drop the"))                      return RUN(K_OPEN, pick_run(OP_OPEN, 0, msg, n));
    if (has(t, "little") || has(t, "nudge") || starts(t, "move") || has(t, "tilt") || has(t, "turn")) {
        int d = has(t, "tilt") ? (has(t, "down") ? NUDGE_TILT_DOWN : NUDGE_TILT_UP)
              : has(t, "turn") ? (has(t, "right") ? NUDGE_TURN_R : NUDGE_TURN_L)
              : has(t, "left") ? NUDGE_LEFT : has(t, "right") ? NUDGE_RIGHT
              : has(t, "up") || has(t, "higher") ? NUDGE_UP : has(t, "down") || has(t, "lower") ? NUDGE_DOWN : -1;
        int kind = d == NUDGE_UP ? K_UP : d == NUDGE_DOWN ? K_DOWN : d == NUDGE_LEFT ? K_LEFT : d == NUDGE_RIGHT ? K_RIGHT
                 : (d == NUDGE_TURN_L || d == NUDGE_TURN_R) ? K_TURN : K_TILT;
        if (d >= 0) return RUN(kind, pick_run(OP_NUDGE, d, msg, n));
    }
    // picking, sorting and stacking need the camera; with it switched off, say so instead
    if (!cam_enabled() && (has(t, "tower") || has(t, "stack") || has(t, "sort") || has(t, "clean up") || has(t, "tidy") ||
                           starts(t, "pick") || starts(t, "grab ") || starts(t, "get ") || starts(t, "fetch"))) {
        fx_say(SND_NO_EYES);
        snprintf(msg, n, "camera is switched off");
        return false;
    }
    // stacking challenge
    if (has(t, "tower") || has(t, "stack"))                         return RUN(K_STACK, pick_run(OP_STACK, 0, msg, n));
    // who's playing: "hi i'm sam", "it's sam"
    if (has(t, "i'm ") || has(t, "i am ") || has(t, "it's ") || has(t, "my name")) {
        int k = kids_find(t);
        if (k >= 0 && (app || school_allow(K_WAVE, msg, n))) { kids_select(k); snprintf(msg, n, "hi %s!", kids_get(k)->name); return pick_run(OP_WAVE, 0, msg, n) || true; }
    }
    if (has(t, "give") || has(t, "hand it") || has(t, "bring it") || has(t, "come here"))                        return RUN(K_GIVE, pick_run(OP_GIVE, 0, msg, n));
    if (has(t, "sort") && (has(t, "size") || has(t, "big") || has(t, "small"))) return RUN(K_SORT, pick_run(OP_SORT_SIZE, 0, msg, n));
    if (has(t, "sort") || has(t, "clean up") || has(t, "tidy"))     return RUN(K_SORT, pick_run(OP_SORT, 0, msg, n));
    if (has(t, "wave") || has(t, "hello") || has(t, "say hi") || has(t, "goodbye") || has(t, "bye"))      return RUN(K_WAVE, pick_run(OP_WAVE, 0, msg, n));
    if (has(t, "bow") || has(t, "thank"))                                              return RUN(K_BOW, pick_run(OP_BOW, 0, msg, n));
    if (has(t, "dance") || has(t, "boogie") || has(t, "party"))                        return RUN(K_DANCE, pick_run(OP_DANCE, 0, msg, n));
    if (has(t, "bell") || has(t, "ring"))                           return RUN(K_BELL, pick_run(OP_BELL, 0, msg, n));
    if (has(t, "home") || has(t, "go back") || has(t, "rest") || has(t, "relax"))                                             return RUN(K_HOME, pick_run(OP_HOME, 0, msg, n));
    if (has(t, "drop") || has(t, "put it down"))                    return RUN(K_DROP, pick_run(OP_DROP, 0, msg, n));
    if (has(t, "open"))                                             return RUN(K_OPEN, pick_run(OP_OPEN, 0, msg, n));
    if (has(t, "close"))                                            return RUN(K_CLOSE, pick_run(OP_CLOSE, 0, msg, n));
    if (starts(t, "pick") || starts(t, "grab") || starts(t, "get") || starts(t, "fetch")) {
        int s = find_slot(t);
        if (s < 0) { snprintf(msg, n, "pick what? I know: %s, %s", g_cfg.slot_name[0], g_cfg.slot_name[1]); return false; }
        return RUN(K_PICK, pick_run(OP_PICK, s, msg, n));
    }
    int m = moves_find(t);                     // "do the robot", "play twirl", or just the name
    if (m >= 0) return RUN(K_PLAY, pick_run(OP_PLAY, m, msg, n));
    snprintf(msg, n, "unknown command");
    return false;
}

int router_phrases(char out[][48], int max)
{
    int n = 0;
#define ADD(...) do { if (n < max) snprintf(out[n++], 48, __VA_ARGS__); } while (0)
    // MultiNet is most reliable on phrases of 3+ syllables that don't sound like each other or like
    // everyday talk. Single words ("wait", "rest", "open", "hello") and near-twins with different
    // actions ("hold it"/"hold this", "come home"/"come here") were dropped from voice; the router
    // still understands them as typed text. "stop" stays short on purpose, with longer backups.
    ADD("stop");
    ADD("stop robot");
    ADD("robot stop");
    ADD("freeze robot");
    ADD("go home");
    ADD("drop it");
    ADD("give it to me");
    if (cam_enabled()) { ADD("sort everything"); ADD("sort by size"); }
    ADD("wave hello");
    ADD("take a bow");
    ADD("dance robot");
    ADD("robot dance");
    ADD("ring the bell");
    ADD("open your hand");
    ADD("close your hand");
    ADD("hold this for me");
    ADD("let go");
    ADD("a little left");
    ADD("a little right");
    ADD("a little up");
    ADD("a little down");
    if (cam_enabled()) ADD("build a tower");
    // more ways to say the same things (each must route through router_exec above)
    ADD("go back");
    ADD("put it down");
    ADD("hand it over");
    ADD("bring it here");
    ADD("come here");
    if (cam_enabled()) { ADD("clean up"); ADD("tidy up"); }
    ADD("say hello");
    ADD("hello robot");
    ADD("goodbye");
    ADD("thank you");
    ADD("bow down");
    ADD("lets dance");
    ADD("party time");
    ADD("grab it");
    ADD("let it go");
    ADD("third hand");
    if (cam_enabled()) ADD("stack them");
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
    // conversation (chat.c). Answers only count right after Muffin asks something.
    ADD("hey muffin"); ADD("hi muffin"); ADD("hello muffin"); ADD("muffin stop"); ADD("stop muffin");
    ADD("whats your name"); ADD("who are you"); ADD("how are you"); ADD("how are you doing");
    ADD("tell me a joke"); ADD("another joke"); ADD("who made you"); ADD("what can you do");
    ADD("good job"); ADD("good robot"); ADD("nice work"); ADD("well done"); ADD("you are awesome");
    ADD("bad robot"); ADD("you are silly"); ADD("i love you"); ADD("are you alive"); ADD("are you a robot");
    ADD("sing a song"); ADD("good night"); ADD("good morning"); ADD("be quiet"); ADD("quiet please");
    ADD("you can talk again"); ADD("surprise me"); ADD("do a trick");
    // teaching (chat.c + pick.c do_teach)
    ADD("teach me about robots"); ADD("teach me something"); ADD("tell me more"); ADD("what is a robot");
    ADD("what is a servo"); ADD("how do you move"); ADD("how do you see"); ADD("how do you hear");
    ADD("how do you think"); ADD("how do you grab"); ADD("tell me a robot fact"); ADD("fun fact");
    ADD("quiz me"); ADD("robot quiz"); ADD("thats true"); ADD("thats false");
    ADD("yes please"); ADD("sure thing"); ADD("no thanks");   // no bare yes/no: one syllable steals matches
    for (int i = 0; i < KIDS_MAX; i++) if (kids_get(i)) ADD("hi i'm %s", kids_get(i)->name);
    if (school_pass()[0]) ADD("%s", school_pass());   // grown-up passphrase (Robot School): ten more minutes
    for (int i = 0; i < NSLOT && cam_enabled(); i++) {
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
