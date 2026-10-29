#include "chat.h"
#include <stdio.h>
#include <string.h>
#include "fx.h"
#include "pick.h"
#include "esp_random.h"
#include "esp_timer.h"

#define ANSWER_US 9000000                // how long a yes/no counts after it asks
#define QUIET_MIN 10

static bool teach(int lesson, char *msg, size_t n);
static bool quiz_start(char *msg, size_t n);
static bool quiz_answer(bool said_true, char *msg, size_t n);

static int s_mood = 1;
static int s_next_lesson;
static struct { int8_t q[3]; int pos, score; int64_t until; } s_quiz = { {0}, -1, 0, 0 };
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
    bool yes = eq(t, "yes") || eq(t, "yes please") || eq(t, "sure") || eq(t, "sure thing") || eq(t, "yeah");
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
    // ---- teaching
    if (has(t, "that's true") || has(t, "thats true") || has(t, "that is true") || eq(t, "true")) return quiz_answer(true, msg, n);
    if (has(t, "that's false") || has(t, "thats false") || has(t, "that is false") || eq(t, "false")) return quiz_answer(false, msg, n);
    if (has(t, "quiz")) return quiz_start(msg, n);
    if (has(t, "fact")) return reply(SND_FACT, msg, n, "fun fact");
    if (has(t, "what is a robot")) return teach(LESSON_ROBOT, msg, n);
    if (has(t, "servo")) return teach(LESSON_SERVO, msg, n);
    if (has(t, "how do you move") || has(t, "joints")) return teach(LESSON_JOINTS, msg, n);
    if (has(t, "how do you grab") || has(t, "gripper")) return teach(LESSON_GRIPPER, msg, n);
    if (has(t, "how do you see") || has(t, "camera")) return teach(LESSON_VISION, msg, n);
    if (has(t, "how do you hear")) return teach(LESSON_HEARING, msg, n);
    if (has(t, "how do you think") || has(t, "brain")) return teach(LESSON_BRAIN, msg, n);
    if (has(t, "teach me") || has(t, "tell me more") || has(t, "lesson")) return teach(s_next_lesson, msg, n);
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

static bool teach(int lesson, char *msg, size_t n)
{
    fx_quiet(0);                              // asking for a lesson is asking it to talk
    s_next_lesson = (lesson + 1) % LESSON_COUNT;
    if (!pick_run(OP_TEACH, lesson, msg, n)) return true;
    snprintf(msg, n, "lesson %d of %d", lesson + 1, LESSON_COUNT);
    return true;
}

void chat_lesson_done(int lesson)
{
    s_next_lesson = (lesson + 1) % LESSON_COUNT;
    ask(SND_ASK_LESSON, OP_TEACH, s_next_lesson);
}

static void quiz_ask(void)
{
    fx_say(QUIZ[s_quiz.q[s_quiz.pos]].q);
    s_quiz.until = esp_timer_get_time() + 30000000;
}

static bool quiz_start(char *msg, size_t n)
{
    fx_quiet(0);
    for (int i = 0; i < 3; i++) {             // three different questions
        int q;
        do { q = esp_random() % QUIZ_COUNT; } while ((i > 0 && q == s_quiz.q[0]) || (i > 1 && q == s_quiz.q[1]));
        s_quiz.q[i] = q;
    }
    s_quiz.pos = 0; s_quiz.score = 0;
    fx_say(SND_QUIZ_INTRO);
    quiz_ask();
    snprintf(msg, n, "quiz: question 1 of 3");
    return true;
}

static bool quiz_answer(bool said_true, char *msg, size_t n)
{
    if (s_quiz.pos < 0 || esp_timer_get_time() > s_quiz.until) { s_quiz.pos = -1; snprintf(msg, n, "no quiz running"); return true; }
    int q = s_quiz.q[s_quiz.pos];
    bool right = said_true == (QUIZ[q].truth != 0);
    fx_say(right ? SND_RIGHT : SND_WRONG);
    fx_say(QUIZ[q].why);
    if (right) { s_quiz.score++; mood(1); }
    if (++s_quiz.pos < 3) { quiz_ask(); snprintf(msg, n, "%s — question %d of 3", right ? "right" : "wrong", s_quiz.pos + 1); return true; }
    if (s_quiz.score) fx_say(SND_QUIZ_SCORE_1 + s_quiz.score - 1); else fx_say(SND_QUIZ_ZERO);
    if (s_quiz.score == 3) fx_play(SND_S_CHEER);
    snprintf(msg, n, "%d of 3 right", s_quiz.score);
    s_quiz.pos = -1;
    return true;
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
