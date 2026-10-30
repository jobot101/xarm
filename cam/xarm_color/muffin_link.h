// muffin_link.h — wireless link to Muffin: the camera joins the board's own WiFi ("Muffin") and sends every
// frame's color blobs to the board as one small UDP packet. The wired I2C link keeps working alongside.
#pragma once
#include "iic_data_send.hpp"

// Muffin's network (net.c on the arm board: SSID "Muffin", CONFIG_XARM_AP_PASS). Change both together.
#define MUFFIN_SSID "Muffin"
#define MUFFIN_PASS "ragamuffin"
#define MUFFIN_PORT 4747

void link_begin(void);
void link_send(const send_color_data_t color[5]);   // called per detection frame; sends at most 25 per second
