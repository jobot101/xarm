// bell.h — a real bell: a solenoid striker on a GPIO (Shop MotherShip: IO3, through a MOSFET with a flyback diode).
// CONFIG_XARM_BELL_GPIO = -1 (Muffin's default) means no solenoid: everything here is a no-op.
//   POST /api/bell {"ms":12, "n":1, "gap":600, "save":false, "listen":true}   ms is capped at 30: taps only
//     strikes n times with an ms-long pulse; "save" keeps ms as the pulse for "ring the bell";
//     "listen" answers with MIC1's loudness (dB per 16 ms hop) from just before the first strike to ~1.5 s after,
//     so the PC can tune the pulse for the loudest, longest ring.
#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

void bell_init(void);              // early in app_main: drives the pin low before anything else runs
bool bell_ready(void);             // a solenoid is configured
int  bell_ms(void);                // the saved pulse length
void bell_strike(int ms);          // one pulse (ms <= 0: the saved length); clamped, rate-limited, thread-safe
void bell_register(httpd_handle_t h);
