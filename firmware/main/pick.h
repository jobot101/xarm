// pick.h — motion sequences (pick / drop / give / sort / tricks / recorded moves). One sequence at a time; stop preempts.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { OP_PICK, OP_HOME, OP_DROP, OP_OPEN, OP_CLOSE,
               OP_GIVE, OP_SORT, OP_WAVE, OP_BOW, OP_DANCE, OP_BELL, OP_PLAY,   // OP_PLAY: slot = move index
               OP_HELPER, OP_NUDGE, OP_STACK, OP_SORT_SIZE, OP_TEACH,
               OP_LESSON, OP_REACT, OP_CELEBRATE } op_t;   // OP_NUDGE: slot = nudge_t, OP_TEACH: slot = lesson_t,
                                   // OP_LESSON: slot = first SCHOOL_STEP | end << 16 (school.c), OP_REACT: 1 yes!, 0 aw, +2 hmm first, 4 just hmm

#define LESSON_EXPLAIN (1 << 30)                      // OP_LESSON flag: tell the whole lesson, no kid's turns

typedef enum { NUDGE_LEFT, NUDGE_RIGHT, NUDGE_UP, NUDGE_DOWN, NUDGE_TURN_L, NUDGE_TURN_R,
               NUDGE_TILT_UP, NUDGE_TILT_DOWN } nudge_t;

void pick_init(void);
bool pick_run(op_t op, int slot, char *msg, size_t n);   // false + msg if busy/invalid
void pick_stop(void);                                    // abort + freeze arm. safe from any task
bool pick_busy(void);
bool pick_idle(void);                                    // no job running or queued
int  pick_lesson_next(void);
void pick_attention(void);                               // someone spoke to it: perk up (idle life)
void pick_face(float dir);
void pick_reach(float k);                                // someone nearby (radar): reach out toward them, 0..1 of the reach
// The chime, played by the arm from the taught Bell pose (the moment of contact).
//   strength 5..25: how far it backs off before each strike, % of the lift. Capped at 25: short fast taps only,
//   a full-distance swing can wreck the chime
//   hit_ms 50..400: the strike's travel time: 50 = full strength (fastest), 400 = gentle; gap_ms 150..3000: one beat
//   pattern: up to 32 of  x strike  ! accent  o soft  . rest   ("xxx" = three even strikes)
typedef struct { int16_t strength, hit_ms, gap_ms; char pattern[33]; } chime_t;
void chime_get(chime_t *c);                   // the saved settings "ring the bell" uses
void chime_save(const chime_t *c);
bool pick_chime(const chime_t *c, char *msg, size_t n);   // play these settings once                               // the voice came from -1 (left) .. 1 (right): turn that way
void pick_listening(bool on);                            // ears open: lean in, head cocked
void pick_anim(bool on);                                 // lifelike motion on/off (saved)
bool pick_anim_on(void);                             // OP_LESSON: the step after the last one it finished
bool pick_holding(void);
bool pick_caught(void);                              // after closing: did the jaws stop on something?
void pick_status(char *state, size_t ns, char *detail, size_t nd);
bool pick_locate(int slot, float *px, float *py);        // averaged blob center, false if not seen
bool pick_look(void);                                   // head cam: move to the look pose (blocking)
