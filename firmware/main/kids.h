// kids.h — who's playing: name, emoji, jewel color, visits and best scores. Lives in the "store" partition.
// The arm greets each kid with their jewel color + their own little bell jingle.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define KIDS_MAX 12

typedef struct {
    char     name[16];      // lowercase, also the voice word ("hi i'm sam")
    char     emoji[12];     // UTF-8, one emoji
    uint8_t  jewel;         // 0 ruby 1 sapphire 2 emerald 3 gold
    uint16_t visits;
    uint16_t best_bells;    // Copy the Bells
    uint16_t best_tower;    // stacking challenge
} kid_t;

void kids_init(void);
int  kids_count(void);
const kid_t *kids_get(int i);             // NULL if empty slot
int  kids_add(const char *name, const char *emoji, int jewel);   // index or -1
bool kids_remove(int i);
bool kids_select(int i);                  // sets current, counts a visit, greets
int  kids_current(void);                  // -1 if nobody picked
int  kids_find(const char *text);         // a kid whose name appears in text, or -1
void kids_score(const char *game, int score);   // "bells" | "tower": keeps the current kid's best
