// pick.h — motion sequences (pick / home / drop). One sequence at a time; stop preempts.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { OP_PICK, OP_HOME, OP_DROP, OP_OPEN, OP_CLOSE } op_t;

void pick_init(void);
bool pick_run(op_t op, int slot, char *msg, size_t n);   // false + msg if busy/invalid
void pick_stop(void);                                    // abort + freeze arm. safe from any task
bool pick_busy(void);
bool pick_holding(void);
void pick_status(char *state, size_t ns, char *detail, size_t nd);
bool pick_locate(int slot, float *px, float *py);        // averaged blob center, false if not seen
