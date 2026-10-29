// host test: cc -I../main test_core.c ../main/core.c -lm -o t && ./t
#include "core.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>

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

    // wrist align: jaws close across X, object 40x10 (long in X) -> turn; 10x40 -> no turn; square -> no turn
    assert(align_twist(40, 10, 1, 375) == 375 && align_twist(10, 40, 1, 375) == 0 && align_twist(20, 20, 1, 375) == 0);
    assert(align_twist(10, 40, 2, -375) == -375 && align_twist(40, 10, 0, 375) == 0);
    printf("ok   align\n");
    // grab check: open 200, close 650. empty hand reaches ~640 -> missed; block stops it at 520 -> held
    assert(grip_missed(640, 650, 200, 40) && !grip_missed(520, 650, 200, 40));
    assert(grip_missed(205, 200, 650, 40) && !grip_missed(330, 200, 650, 40));   // reversed gripper direction
    printf("ok   grab check\n");
    assert(size_bin(20, 20, 900) == 0 && size_bin(40, 30, 900) == 1);
    printf("ok   size bin\n");

    // min-jerk: fixed ends, symmetric, monotonic
    assert(ease_minjerk(0) == 0 && ease_minjerk(1) == 1 && fabsf(ease_minjerk(0.5f) - 0.5f) < 1e-6f);
    for (int i = 1; i <= 100; i++) assert(ease_minjerk(i / 100.0f) >= ease_minjerk((i - 1) / 100.0f));
    assert(ease_minjerk(0.02f) < 0.001f);              // gentle start
    printf("ok   ease\n");

    // follow: 0 -> 400 at 25 Hz, vmax 1000, amax 4000: lands exactly, never overshoots, accel limited
    {
        float pos = 0, vel = 0, prev = 0, dt = 0.04f; int t = 0; bool done = false;
        for (; t < 200 && !done; t++) {
            done = follow_step(&pos, &vel, 400, 1000, 4000, dt);
            assert(pos <= 400.0f + 1e-3f && vel >= -1e-3f && vel <= 1000.0f + 1e-3f);
            assert(fabsf(vel - prev) <= 4000 * dt + 1e-2f || done);
            prev = vel;
        }
        assert(done && pos == 400 && vel == 0 && t < 30);   // ~0.7 s, not a crawl
        // reversing mid-flight: target behind it -> slows, turns around, lands
        pos = 0; vel = 800; done = false;
        for (t = 0; t < 200 && !done; t++) done = follow_step(&pos, &vel, -50, 1000, 4000, dt);
        assert(done && pos == -50);
        // tiny step settles
        pos = 100; vel = 0;
        for (t = 0; t < 10 && !follow_step(&pos, &vel, 101, 1000, 4000, dt); t++) {}
        assert(pos == 101);
    }
    printf("ok   follow\n");

    // velocity: ramps up at amax, holds, ramps down to a stop
    {
        float pos = 0, vel = 0;
        vel_step(&pos, &vel, 300, 4000, 0.04f); assert(fabsf(vel - 160) < 1e-3f);
        vel_step(&pos, &vel, 300, 4000, 0.04f); assert(fabsf(vel - 300) < 1e-3f);
        for (int i = 0; i < 5; i++) vel_step(&pos, &vel, 0, 4000, 0.04f);
        assert(vel == 0 && pos > 0);
    }
    printf("ok   velocity\n");

    // catmull: passes through p1 at 0 and p2 at 1, straight line stays straight
    assert(catmull(0, 100, 200, 300, 0) == 100 && catmull(0, 100, 200, 300, 1) == 200);
    assert(fabsf(catmull(0, 100, 200, 300, 0.5f) - 150) < 1e-3f);
    printf("ok   catmull\n");

    // cloud transcripts -> router text
    {
        char o[96]; int f;
        f = text_clean("Hey Muffin, can you pick up the RED one, please?", o, sizeof o);
        assert(f == TC_WAKE && !strcmp(o, "pick up the red one"));
        f = text_clean("Hey, Muffin!", o, sizeof o);          assert(f == TC_WAKE && !strcmp(o, ""));
        f = text_clean("muffin", o, sizeof o);                assert(f == TC_WAKE && !strcmp(o, ""));
        f = text_clean("Dance, robot!", o, sizeof o);         assert(f == 0 && !strcmp(o, "dance robot"));
        text_clean("Stop, Muffin!", o, sizeof o);             assert(!strcmp(o, "stop"));
        text_clean("Muffin, stop.", o, sizeof o);             assert(!strcmp(o, "stop"));
        text_clean("Yeah, sure!", o, sizeof o);               assert(!strcmp(o, "yes"));
        text_clean("Yes please.", o, sizeof o);               assert(!strcmp(o, "yes please"));
        text_clean("Okay.", o, sizeof o);                     assert(!strcmp(o, "yes"));
        text_clean("Nah.", o, sizeof o);                      assert(!strcmp(o, "no"));
        text_clean("No thanks", o, sizeof o);                 assert(!strcmp(o, "no thanks"));
        text_clean("Muffin, yes.", o, sizeof o);              assert(!strcmp(o, "yes"));
        text_clean("That’s true!", o, sizeof o);         assert(!strcmp(o, "that's true"));
        text_clean("Um, so, could you please sort everything?", o, sizeof o); assert(!strcmp(o, "sort everything"));
        text_clean("Okay Muffin, tell me a joke.", o, sizeof o); assert(!strcmp(o, "tell me a joke"));
        text_clean("Hold this for me.", o, sizeof o);         assert(!strcmp(o, "hold this for me"));
        text_clean("Sort by size", o, sizeof o);              assert(!strcmp(o, "sort by size"));   // "so" is a whole word only
        text_clean("Hey", o, sizeof o);                       assert(!strcmp(o, "hey"));
        text_clean("  ", o, sizeof o);                        assert(!strcmp(o, "") && text_junk(o));
        text_clean("Thanks for watching!", o, sizeof o);      assert(text_junk(o));
        text_clean("Thank you.", o, sizeof o);                assert(!text_junk(o) && !strcmp(o, "thank you"));
        text_clean("Pick up the red one now please", o, sizeof o); assert(!strcmp(o, "pick up the red one"));
        char tiny[8]; text_clean("pick up the red one", tiny, sizeof tiny); assert(strlen(tiny) < 8);
    }
    printf("ok   text_clean\n");

    puts("all passed");
    return 0;
}
