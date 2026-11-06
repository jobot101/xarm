// cam.h — Hiwonder ESP32-S3 vision module (color mode) over I2C, polled at 10 Hz.
#pragma once
#include "core.h"
#include "driver/i2c_master.h"

esp_err_t cam_begin(i2c_master_bus_handle_t bus);
bool cam_get(int slot, box_t *out);     // latest reading; returns true if a blob is in view
bool cam_ok(void);                       // I2C talking
bool cam_enabled(void);                  // the camera switch (Workshop -> Settings), read at boot
void cam_enable_save(bool on);           // saved; takes effect after a restart
void cam_raw_hex(char *out, size_t n);   // last raw bytes, for checking the data format
int  cam_scan(char *out, size_t n);
// MotherShip's ribbon camera (eye.c): the PC finds the colors and posts the blobs here. cam_ok() is then true while
// they keep arriving (1.5 s).
void cam_feed(int slot, const box_t *b, bool seen);      // probe the I2C bus -> "0x18 0x20 ..."; returns count
