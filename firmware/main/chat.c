#include "chat.h"
#include <stdio.h>
#include <string.h>
#include "fx.h"
#include "pick.h"
#include "esp_random.h"
#include "esp_timer.h"

#define ANSWER_US 9000000                // how long a yes/no counts after it asks
#define QUIET_MIN 10

static int s_mood = 1;
static int s_tricks;
static struct { int op, slot; int64_t until; } s_ask = { -1, 0, 0 };

static bool eq(const char *a, const char *b) { return !strcmp(a, b); }
static bool has(const char *s, const char *w) { return strstr(s, w) != NULL; }
static void mood(int d) { s_mood += d; if (s_mood > 3) s_mood = 3; if (s_mood < -3) s_mood = -3; }
static int pct(int p) { return (int)(esp_random() % 100) < p; }
int chat_mood(void) { return s_mood; }
void chat_stopped(void) { mood(-1); }

static void ask(int group, int op, int slot)
{
    fx_say(group);
    s_ask.op = op; s_ask.slot = slot;
    s_ask.until = esp_timer_get_time() + ANSWER_US;
}

static int any_trick(void)
{
    static const int T[] = { OP_WAVE, OP_DANCE, OP_BOW, OP_DANCE };
    return T[esp_random() % 4];
}

static bool reply(int group, char *msg, size_t n, const char *what)
{
    fx_say(group);
    snprintf(msg, n, "%s", what);
    return true;
}

bool chat_exec(const char *t, char *msg, size_t n)
{
    // ---- answers to the question it just asked
    bool yes = eq(t, "yes") || eq(t, "yes please") || eq(t, "sure") || eq(t, "yeah");
    bool no  = eq(t, "no") || eq(t, "no thanks") || eq(t, "nope");
    if (yes || no) {
        bool pending = s_ask.op >= 0 && esp_timer_get_time() < s_ask.until;
        int op = s_ask.op, slot = s_ask.slot;
        s_ask.op = -1;
        if (!pending) { snprintf(msg, n, "nothing to answer"); return true; }   // stray yes/no: ignore quietly
        if (no) return reply(SND_NO, msg, n, "fine");
        mood(1);
        pick_run((op_t)op, slot, msg, n);
        return true;
    }
    // ---- its name
    if (has(t, "muffin") && (eq(t, "muffin") || has(t, "hey") || has(t, "hi ") || has(t, "hello")))
        return reply(SND_HEY, msg, n, "you rang?");
    if (has(t, "your name") || has(t, "who are you")) return reply(SND_NAME, msg, n, "I'm Muffin");
    // ---- small talk
    if (has(t, "how are you")) return reply(s_mood >= 0 ? SND_HOW_GOOD : SND_HOW_BAD, msg, n, s_mood >= 0 ? "great" : "grumpy");
    if (has(t, "joke")) {
        fx_say(SND_JOKE);
        if (pct(60)) fx_say(SND_JOKE_TAG);
        snprintf(msg, n, "ba dum tss");
        return true;
    }
    if (has(t, "who made you") || has(t, "who built you")) return reply(SND_MAKER, msg, n, "Ragamuffin Studios");
    if (has(t, "what can you do")) return reply(SND_CAN_DO, msg, n, "lots");
    if (has(t, "good job") || has(t, "good robot") || has(t, "nice work") || has(t, "well done") ||
        has(t, "awesome") || has(t, "good muffin")) { mood(1); return reply(SND_PRAISE, msg, n, "I know"); }
    if (has(t, "bad robot") || has(t, "bad muffin") || has(t, "silly")) { mood(-2); return reply(SND_SCOLD, msg, n, "rude"); }
    if (has(t, "love you")) { mood(1); return reply(SND_LOVE, msg, n, "of course"); }
    if (has(t, "are you alive") || has(t, "are you a robot")) return reply(SND_ALIVE, msg, n, "big personality");
    if (has(t, "sing")) { fx_say(SND_SING); fx_play(SND_S_DANCE); snprintf(msg, n, "♪"); return true; }
    if (has(t, "good night")) return reply(SND_NIGHT, msg, n, "good night");
    if (has(t, "good morning")) return reply(SND_MORNING, msg, n, "good morning");
    // ---- social cues
    if (has(t, "be quiet") || has(t, "quiet please") || eq(t, "shh")) {
        fx_say(SND_QUIET);
        fx_quiet(QUIET_MIN);
        snprintf(msg, n, "quiet for %d min", QUIET_MIN);
        return true;
    }
    if (has(t, "you can talk")) { fx_quiet(0); return reply(SND_TALK_AGAIN, msg, n, "finally"); }
    // ---- surprise
    if (has(t, "surprise me") || has(t, "do a trick")) {
        fx_say(SND_SURPRISE);
        pick_run((op_t)any_trick(), 0, msg, n);
        return true;
    }
    return false;
}

void chat_idle(void)
{
    if (pct(50)) ask(SND_ASK_TRICK, any_trick(), 0);
    else fx_say(SND_IDLE);
}

void chat_show_done(int op, int slot)
{
    s_tricks++;
    if (s_tricks % 10 == 0) { fx_say(SND_MILESTONE); return; }
    bool encore = op == OP_WAVE || op == OP_BOW || op == OP_DANCE || op == OP_BELL || op == OP_PLAY;
    if (encore && pct(22)) ask(SND_ASK_MORE, op, slot);
}
