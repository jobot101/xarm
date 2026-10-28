// voice.h — offline speech commands: ES7210 mic -> ESP-SR MultiNet (English) -> router.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "driver/i2c_master.h"

void voice_start(i2c_master_bus_handle_t bus);
void voice_reload(void);                  // call after slot names change
bool voice_ok(void);
void voice_last(char *out, size_t n);     // last heard phrase
