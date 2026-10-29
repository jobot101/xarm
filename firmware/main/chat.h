// chat.h — Muffin's conversation tricks. All offline: the recognizer only knows fixed phrases, so the
// "conversation" is a bag of cheap, effective hacks that make a fixed vocabulary feel like talking:
//   - answers to its name, small talk, jokes (never the same one twice in a row)
//   - mood: praise cheers it up, scolding and "stop" sour it; "how are you" tells you which
//   - it asks yes/no questions ("want to see a trick?") and acts on a yes for ~9 s
//   - call-backs: "again?" on repeats, milestones as tricks pile up
//   - social cues: "be quiet" really quiets it for 10 minutes; "you can talk again" lifts it
#pragma once
#include <stdbool.h>
#include <stddef.h>

bool chat_exec(const char *text, char *msg, size_t n);   // lower-case text; true if it was conversation
void chat_idle(void);                    // idle nudge: a line, or a question it'll act on
void chat_show_done(int op, int slot);   // a trick finished: maybe offer an encore, count milestones
void chat_stopped(void);                 // someone said stop
int  chat_mood(void);                    // -3 grumpy .. 3 delighted
void chat_lesson_done(int lesson);       // offers the next lesson
// Teaching: "teach me about robots" / "tell me more" walk the lessons in order (each one demonstrated on
// the arm, pick.c do_teach); "what is a servo", "how do you see"... jump to one; "fun fact"; "quiz me"
// asks three true/false questions and keeps score.
