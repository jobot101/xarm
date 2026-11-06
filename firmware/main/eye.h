// eye.h — Johnny: the audio board's own DVP camera (the ribbon connector on the back).
//
// The board only takes pictures. The shop PC (pc/eye.py) pulls them, finds the slot colors and posts the blobs
// back, and cam.c serves those blobs to pick.c exactly as if the Hiwonder vision module had sent them over I2C.
//   GET  /cam.jpg     latest frame, 320x240 JPEG
//   POST /api/blobs   {"blobs": [[x,y,w,h] | null, ...]}  one per slot, centre x,y and size on the 240x240 frame
// On the Muffin build these are stubs and nothing here is linked.
#pragma once
#include <stdbool.h>
#include "esp_http_server.h"
#include "driver/i2c_master.h"

bool eye_init(i2c_master_bus_handle_t bus);   // wakes the camera; true if a sensor answered
bool eye_ok(void);
const char *eye_sensor(void);                 // "OV2640", ... or "none"
void eye_register(httpd_handle_t h);          // adds /cam.jpg and /api/blobs
void eye_pause(bool on);                      // stop capturing (nobody here): frees CPU, /cam.jpg answers 503
