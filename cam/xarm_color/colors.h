// colors.h — which two colors the arm's main board sees as "slot 0" (I2C reg 0x00) and "slot 1" (reg 0x01).
// 0 red, 1 yellow, 2 green, 3 blue, 4 purple. All five are also readable at regs 0x10..0x14.
// HSV thresholds for each color live in color_detection.cpp (std_color_info).
#pragma once
#ifndef SLOT0_COLOR
#define SLOT0_COLOR 0
#endif
#ifndef SLOT1_COLOR
#define SLOT1_COLOR 3
#endif
