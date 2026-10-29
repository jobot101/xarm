#include "core.h"
#include <math.h>
#include <string.h>
#include <ctype.h>

#define LO(v) ((uint8_t)((v) & 0xFF))
#define HI(v) ((uint8_t)(((v) >> 8) & 0xFF))

uint16_t clamp_pos(int v) { return v < POS_MIN ? POS_MIN : v > POS_MAX ? POS_MAX : (uint16_t)v; }

size_t xf_move(uint8_t *o, const uint8_t *ids, const uint16_t *pos, int n, uint16_t ms)
{
    size_t i = 0;
    o[i++] = 0x55; o[i++] = 0x55;
    o[i++] = (uint8_t)(n * 3 + 5);
    o[i++] = XCMD_SERVO_MOVE;
    o[i++] = (uint8_t)n;
    o[i++] = LO(ms); o[i++] = HI(ms);
    for (int k = 0; k < n; k++) {
        uint16_t p = clamp_pos(pos[k]);
        o[i++] = ids[k]; o[i++] = LO(p); o[i++] = HI(p);
    }
    return i;
}

size_t xf_ids(uint8_t *o, uint8_t cmd, const uint8_t *ids, int n)
{
    size_t i = 0;
    o[i++] = 0x55; o[i++] = 0x55;
    o[i++] = (uint8_t)(n + 3);
    o[i++] = cmd;
    o[i++] = (uint8_t)n;
    for (int k = 0; k < n; k++) o[i++] = ids[k];
    return i;
}

size_t xf_simple(uint8_t *o, uint8_t cmd)
{
    o[0] = 0x55; o[1] = 0x55; o[2] = 2; o[3] = cmd;
    return 4;
}

size_t xf_action(uint8_t *o, uint8_t group, uint16_t times)
{
    o[0] = 0x55; o[1] = 0x55; o[2] = 5; o[3] = XCMD_ACTION_RUN;
    o[4] = group; o[5] = LO(times); o[6] = HI(times);
    return 7;
}

bool xp_feed(xparser_t *p, uint8_t b)
{
    switch (p->st) {
    case 0: if (b == 0x55) p->st = 1; break;
    case 1: p->st = (b == 0x55) ? 2 : 0; break;
    case 2:                                   // LEN
        if (b < 2 || b - 2 > (int)sizeof p->p) { p->st = (b == 0x55) ? 1 : 0; break; }
        p->need = b - 2; p->got = 0; p->st = 3; break;
    case 3:                                   // CMD
        p->cmd = b;
        if (p->need == 0) { p->np = 0; p->st = 0; return true; }
        p->st = 4; break;
    case 4:
        p->p[p->got++] = b;
        if (p->got >= p->need) { p->np = p->got; p->st = 0; return true; }
        break;
    }
    return false;
}

int xp_positions(const xparser_t *p, uint16_t pos[NSERVO])
{
    if (p->cmd != XCMD_POS_READ || p->np < 1) return 0;
    int n = p->p[0], got = 0;
    for (int k = 0; k < n && 1 + k * 3 + 2 < p->np; k++) {
        uint8_t id = p->p[1 + k * 3];
        int v = p->p[2 + k * 3] | (p->p[3 + k * 3] << 8);
        if (id >= 1 && id <= NSERVO) { pos[id - 1] = clamp_pos(v); got++; }
    }
    return got;
}

bool cam_decode(const uint8_t r[CAM_BYTES], box_t *b)
{
    b->x = r[0]; b->y = r[1]; b->w = r[2]; b->h = r[3];
    return b->w > 0 && b->h > 0;
}

void interp_pose(const calib_pt_t *pts, int n, float px, float py, uint16_t out[NSERVO])
{
    float acc[NSERVO] = {0}, wsum = 0;
    for (int i = 0; i < n; i++) {
        float dx = px - pts[i].px, dy = py - pts[i].py, d2 = dx * dx + dy * dy;
        if (d2 < 1.0f) {                              // on a taught point: use it exactly
            for (int j = 0; j < NSERVO; j++) out[j] = pts[i].pos[j];
            return;
        }
        float w = 1.0f / d2;                          // IDW, power 2
        wsum += w;
        for (int j = 0; j < NSERVO; j++) acc[j] += w * pts[i].pos[j];
    }
    for (int j = 0; j < NSERVO; j++) out[j] = clamp_pos((int)lroundf(acc[j] / wsum));
}

int align_twist(int w, int h, int jaws_axis, int turn)
{
    bool long_x = w * 10 >= h * 14, long_y = h * 10 >= w * 14;
    if ((jaws_axis == 1 && long_x) || (jaws_axis == 2 && long_y)) return turn;
    return 0;
}

bool grip_missed(int actual, int grip_close, int grip_open, int margin)
{
    int dir = grip_close > grip_open ? 1 : -1;          // closing direction in servo counts
    return (grip_close - actual) * dir < margin;        // stopped short of closed = something's in there
}

int size_bin(int w, int h, int split) { return w * h >= split; }

// ---- smooth motion ----
float ease_minjerk(float s)
{
    if (s <= 0) return 0;
    if (s >= 1) return 1;
    return s * s * s * (10.0f + s * (-15.0f + 6.0f * s));
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

bool follow_step(float *pos, float *vel, float target, float vmax, float amax, float dt)
{
    float e = target - *pos, dv = amax * dt;
    if (fabsf(e) < 0.5f && fabsf(*vel) <= dv) { *pos = target; *vel = 0; return true; }
    // fastest speed that can still brake to zero in the distance left (counting this tick's travel)
    float room = fabsf(e) - fabsf(*vel) * dt * 0.5f;
    float vb = sqrtf(2.0f * amax * (room > 0 ? room : 0));
    float vdes = copysignf(vb < vmax ? vb : vmax, e);
    *vel += clampf(vdes - *vel, -dv, dv);
    *pos += *vel * dt;
    if ((target - *pos) * e <= 0) { *pos = target; *vel = 0; return true; }   // crossed it: land
    return false;
}

void vel_step(float *pos, float *vel, float vdes, float amax, float dt)
{
    float dv = amax * dt;
    *vel += clampf(vdes - *vel, -dv, dv);
    *pos += *vel * dt;
}

float catmull(float p0, float p1, float p2, float p3, float t)
{
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * (2 * p1 + (p2 - p0) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (3 * p1 - p0 - 3 * p2 + p3) * t3);
}

// ---------------------------------------------------------------- cloud speech text
static bool word_at(const char *s, const char *w)        // s starts with whole word(s) w
{
    size_t k = strlen(w);
    return !strncmp(s, w, k) && (s[k] == 0 || s[k] == ' ');
}

static bool ends_word(const char *s, size_t len, const char *w, size_t *cut)   // s[0..len) ends with " w" or is w
{
    size_t k = strlen(w);
    if (len < k || strncmp(s + len - k, w, k)) return false;
    if (len > k && s[len - k - 1] != ' ') return false;
    *cut = len - k;
    return true;
}

static const char *const YES[] = { "yes", "yeah", "yep", "yup", "sure", "ok", "okay", "of course", "absolutely",
    "uh huh", "yes please", "sure thing", "yeah sure", "ok sure", "okay sure", "yes yes", "definitely", "please", 0 };
static const char *const NO[]  = { "no", "nope", "nah", "no thanks", "no thank you", "no way", "not now", 0 };

static bool in_list(const char *s, const char *const *l) { for (; *l; l++) if (!strcmp(s, *l)) return true; return false; }

// "yes please" / "sure thing" / "no thanks" are chat.c phrases already; the rest collapse to yes / no
static void fold_answer(char *s, size_t n)
{
    const char *to = NULL;
    if (in_list(s, YES)) to = strcmp(s, "yes please") && strcmp(s, "sure thing") ? "yes" : NULL;
    else if (in_list(s, NO)) to = strcmp(s, "no thanks") ? "no" : NULL;
    if (to && n > strlen(to)) strcpy(s, to);
}

int text_clean(const char *in, char *out, size_t n)
{
    if (!n) return 0;
    // 1. lower-case; anything but letters, digits and apostrophes becomes one space
    size_t k = 0;
    for (const char *p = in; *p && k + 1 < n; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == 0xE2 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x99) { out[k++] = '\''; p += 2; continue; }   // ’
        if (isalnum(c) || c == '\'') out[k++] = (char)tolower(c);
        else if (k && out[k - 1] != ' ') out[k++] = ' ';
    }
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;

    int flags = 0;
    for (char *m = out; (m = strstr(m, "muffin")); m++)
        if (m == out || m[-1] == ' ') { flags |= TC_WAKE; break; }

    // 2. whole-answer folding first ("okay" alone is a yes, but "okay muffin dance" is a command)
    fold_answer(out, n);
    if (!strcmp(out, "yes") || !strcmp(out, "no") || !strcmp(out, "yes please") || !strcmp(out, "sure thing") || !strcmp(out, "no thanks"))
        return flags;

    // 3. strip leading wake words and filler, repeatedly
    static const char *const LEAD[] = { "hey muffin", "hi muffin", "hello muffin", "okay muffin", "ok muffin", "yo muffin",
        "muffins", "muffin", "hey", "um", "uh", "umm", "uhh", "so", "okay", "ok", "now", "please", "can you please",
        "could you please", "can you", "could you", "would you", "will you", "i want you to", "go ahead and", "i said",
        "i need you to", "you should", 0 };
    char *s = out;
    for (bool again = true; again; ) {
        again = false;
        for (const char *const *l = LEAD; *l; l++) {
            if (word_at(s, *l)) {
                size_t w = strlen(*l);
                // a lone "hey" / "okay" isn't filler, it's the whole thing they said
                if (s[w] == 0 && (!strcmp(*l, "hey") || !strcmp(*l, "okay") || !strcmp(*l, "ok"))) continue;
                s += w;
                while (*s == ' ') s++;
                again = true;
                break;
            }
        }
    }
    // 4. strip trailing politeness / name
    size_t len = strlen(s), cut;
    for (bool again = true; again; ) {
        again = false;
        static const char *const TAIL[] = { "please", "muffin", "muffins", "now", "thanks", 0 };
        for (const char *const *l = TAIL; *l; l++)
            if (len > strlen(*l) && ends_word(s, len, *l, &cut)) { len = cut; while (len && s[len - 1] == ' ') len--; again = true; }
    }
    memmove(out, s, len);
    out[len] = 0;
    fold_answer(out, n);
    return flags;
}

bool text_junk(const char *t)
{
    static const char *const J[] = { "", "you", "thanks for watching", "thank you for watching", "please subscribe",
        "subscribe", "music", "applause", "silence", "the end", "bye bye bye", 0 };
    return in_list(t, J);
}
