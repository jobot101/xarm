// cam.h — Hiwonder ESP32-S3 vision module (color mode) over I2C, polled at 10 Hz.
#pragma once
#include "core.h"
#include "driver/i2c_master.h"

esp_err_t cam_init(i2c_master_bus_handle_t bus);
bool cam_get(int slot, box_t *out);     // latest reading; returns true if a blob is in view
bool cam_ok(void);                       // I2C talking
void cam_raw_hex(char *out, size_t n);   // last raw bytes, for checking the data format
int  cam_scan(char *out, size_t n);      // probe the I2C bus -> "0x18 0x20 ..."; returns count
