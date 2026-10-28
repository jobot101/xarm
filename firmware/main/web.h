// web.h — HTTP server: serves the React app (gzipped, embedded) + JSON API. See README "API".
#pragma once
#include <stdbool.h>
bool web_start(void);     // false if the HTTP server didn't start
