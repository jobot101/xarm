// router.h — one command vocabulary for voice AND the web app.
// Add a command here once and both inputs get it.
#pragma once
#include <stdbool.h>
#include <stddef.h>

// run a plain-text command ("pick up the red", "stop", ...). msg gets a short reply.
bool router_exec(const char *text, char *msg, size_t n);

// the exact phrases the voice recognizer listens for (rebuilt when slot names change)
int router_phrases(char out[][48], int max);
