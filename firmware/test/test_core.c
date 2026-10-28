// host test: cc -I../main test_core.c ../main/core.c -lm -o t && ./t
#include "core.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static void eq(const uint8_t *a, const uint8_t *b, size_t n, const char *what)
{
    if (memcmp(a, b, n)) {
        printf("FAIL %s\n got:", what);
        for (size_t i = 0; i < n; i++) printf(" %02X", a[i]);
        printf("\n want:");
        for (size_t i = 0; i < n; i++) printf(" %02X", b[i]);
        printf("\n");
        assert(0);
    }
    printf("ok   %s\n", what);
}

int main(void)
{
    uint8_t o[64];

    // Hiwonder doc example: servo 1 -> 800 in 1000 ms
    uint8_t id1[] = {1}; uint16_t p800[] = {800};
    size_t n = xf_move(o, id1, p800, 1, 1000);
    eq(o, (uint8_t[]){0x55,0x55,0x08,0x03,0x01,0xE8,0x03,0x01,0x20,0x03}, n, "move 1 servo");

    // doc example: unload 1..6
    uint8_t ids[] = {1,2,3,4,5,6};
    n = xf_ids(o, XCMD_UNLOAD, ids, 6);
    eq(o, (uint8_t[]){0x55,0x55,0x09,0x14,0x06,1,2,3,4,5,6}, n, "unload 6");

    n = xf_simple(o, XCMD_BATTERY);
    eq(o, (uint8_t[]){0x55,0x55,0x02,0x0F}, n, "battery req");

    // doc example reply: read 6 servos, all 500
    uint8_t reply[] = {0x00,0x55,0x55,0x15,0x15,0x06,
        0x01,0xF4,0x01, 0x02,0xF4,0x01, 0x03,0xF4,0x01,
        0x04,0xF4,0x01, 0x05,0xF4,0x01, 0x06,0xF4,0x01};
    xparser_t p = {0}; int frames = 0; uint16_t pos[NSERVO] = {0};
    for (size_t i = 0; i < sizeof reply; i++)
        if (xp_feed(&p, reply[i])) { frames++; assert(xp_positions(&p, pos) == 6); }
    assert(frames == 1);
    for (int i = 0; i < NSERVO; i++) assert(pos[i] == 500);
    printf("ok   parse pos reply\n");

    // battery reply 7500 mV, then a bare action-stop frame
    uint8_t bat[] = {0x55,0x55,0x04,0x0F,0x4C,0x1D, 0x55,0x55,0x02,0x07};
    p = (xparser_t){0}; frames = 0;
    for (size_t i = 0; i < sizeof bat; i++) if (xp_feed(&p, bat[i])) {
        frames++;
        if (p.cmd == XCMD_BATTERY) assert((p.p[0] | p.p[1] << 8) == 7500);
        else assert(p.cmd == XCMD_ACTION_STOP && p.np == 0);
    }
    assert(frames == 2);
    printf("ok   parse battery + action stop\n");

    box_t b; uint8_t raw[CAM_BYTES] = {200, 60, 20, 18};      // uint8 x,y,w,h on a 240x240 frame
    assert(cam_decode(raw, &b) && b.x == 200 && b.y == 60 && b.w == 20 && b.h == 18);
    memset(raw, 0, CAM_BYTES); assert(!cam_decode(raw, &b));
    printf("ok   cam decode\n");

    calib_pt_t c[2] = {{0, 0, {100,100,100,100,100,100}}, {100, 0, {300,300,300,300,300,300}}};
    uint16_t out[NSERVO];
    interp_pose(c, 2, 50, 0, out);  assert(out[5] == 200);
    interp_pose(c, 2, 100, 0, out); assert(out[5] == 300);
    interp_pose(c, 2, 25, 0, out);  assert(out[5] == 120);   // w 1/625 vs 1/5625 -> 0.9*100+0.1*300
    printf("ok   interp\n");

    puts("all passed");
    return 0;
}
