// pick.h — motion sequences (pick / drop / give / sort / tricks / recorded moves). One sequence at a time; stop preempts.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { OP_PICK, OP_HOME, OP_DROP, OP_OPEN, OP_CLOSE,
               OP_GIVE, OP_SORT, OP_WAVE, OP_BOW, OP_DANCE, OP_BELL, OP_PLAY,   // OP_PLAY: slot = move index
               OP_HELPER, OP_NUDGE, OP_STACK, OP_SORT_SIZE } op_t;                            // OP_NUDGE: slot = nudge_t

typedef enum { NUDGE_LEFT, NUDGE_RIGHT, NUDGE_UP, NUDGE_DOWN, NUDGE_TURN_L, NUDGE_TURN_R,
               NUDGE_TILT_UP, NUDGE_TILT_DOWN } nudge_t;

void pick_init(void);
bool pick_run(op_t op, int slot, char *msg, size_t n);   // false + msg if busy/invalid
void pick_stop(void);                                    // abort + freeze arm. safe from any task
bool pick_busy(void);
bool pick_holding(void);
void pick_status(char *state, size_t ns, char *detail, size_t nd);
bool pick_locate(int slot, float *px, float *py);        // averaged blob center, false if not seen
bool pick_look(void);                                   // head cam: move to the look pose (blocking)
