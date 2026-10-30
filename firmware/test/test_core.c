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

    // Robot School helpers
    assert(civil_days(1970, 1, 1) == 0);
    assert(civil_days(2000, 3, 1) - civil_days(2000, 2, 28) == 2);            // 2000 was a leap year
    assert(civil_days(1900, 3, 1) - civil_days(1900, 2, 28) == 1);            // 1900 wasn't
    assert(civil_days(2027, 1, 1) - civil_days(2026, 12, 31) == 1);           // yesterday across New Year
    assert(civil_days(2026, 10, 29) == 20755);
    assert(streak_next(100, 101, 4) == 5 && streak_next(100, 102, 4) == 1);
    assert(streak_next(101, 101, 5) == 5 && streak_next(101, 101, 0) == 1 && streak_next(-3, 200, 9) == 1);
    assert(pass_heard("hey muffin purple elephant", "Purple Elephant"));
    assert(pass_heard("Purple, elephant!", "purple elephant"));
    assert(!pass_heard("purple elephants", "purple elephant"));               // whole words only
    assert(!pass_heard("purple", "purple elephant"));
    assert(!pass_heard("anything at all", "") && !pass_heard("go home", "home"));   // too short to unlock
    assert(pass_heard("thats my secret", "that's my secret"));
    assert(kind_fits(K_NUDGE, K_UP) && kind_fits(K_NUDGE, K_TILT) && !kind_fits(K_NUDGE, K_DANCE));
    assert(kind_fits(K_UP, K_UP) && !kind_fits(K_UP, K_DOWN) && !kind_fits(K_NONE, K_NONE));
    assert(kind_fits(K_FETCH, K_CLOSE) && !kind_fits(K_FETCH, K_OPEN) && kind_fits(K_GIVEBACK, K_OPEN) && !kind_fits(K_GIVEBACK, K_CLOSE));
    {
        uint8_t m[4] = {0};
        assert(recap_pick(m, 0, 7, -1) == -1 && recap_pick(m, 1, 7, 0) == 0);
        m[1] = 1 << 2;                                                         // question 10 was missed
        for (uint32_t r = 0; r < 20; r++) assert(recap_pick(m, 12, r, -1) == 10);
        for (uint32_t r = 0; r < 20; r++) { int q = recap_pick(m, 12, r, 10); assert(q >= 0 && q < 12 && q != 10); }
        assert(recap_pick(m, 5, 3, -1) >= 0 && recap_pick(m, 5, 3, -1) < 5);  // missed one not learned yet
        for (uint32_t r = 0; r < 20; r++) assert(recap_pick(NULL, 3, r, 1) != 1);
    }
    assert(rank_of(0) == 0 && rank_of(4) == 0 && rank_of(5) == 1 && rank_of(43) == 8);
    printf("ok   school\n");

    // lifelike travel: starts on from, ends on to, bulges mid-flight, lagging joints start late
    {
        uint16_t a[NSERVO] = {200, 500, 500, 500, 500, 300}, b[NSERVO] = {200, 500, 500, 500, 500, 700}, o[NSERVO];
        int16_t bu[NSERVO] = {0, 0, 0, 0, 40, 0};
        uint16_t lag[NSERVO] = {200, 190, 160, 110, 60, 0};
        arc_pose(a, b, bu, lag, 1000, 0, o);    assert(!memcmp(o, a, sizeof o));
        arc_pose(a, b, bu, lag, 1000, 1200, o); assert(!memcmp(o, b, sizeof o));
        arc_pose(a, b, bu, lag, 1000, 560, o);  assert(o[4] >= 538 && o[4] <= 540);   // shoulder: mid-flight, full bulge
        assert(o[5] > 500 && o[5] < 700);       // base well on its way
        arc_pose(a, b, bu, lag, 1000, 100, o);  assert(o[4] == 500 && o[5] > 300);     // shoulder hasn't started, base has
        uint16_t prev = 300;                    // base never turns back
        for (int t = 0; t <= 1200; t += 40) { arc_pose(a, b, bu, lag, 1000, t, o); assert(o[5] >= prev); prev = o[5]; }
        arc_pose(a, b, NULL, NULL, 0, 0, o);    assert(!memcmp(o, b, sizeof o));          // zero duration = there
    }
    printf("ok   arc\n");

    // pink noise: bounded, wanders, not white
    {
        pink_t pk = {0}; float mn = 9, mx = -9, d = 0, prev = 0; uint32_t r = 12345;
        for (int i = 0; i < 4000; i++) {
            r = r * 1103515245u + 12345u;
            float v = pink_next(&pk, r);
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            if (i) d += fabsf(v - prev);
            prev = v;
        }
        assert(mn > -2.1f && mx < 2.1f && mx - mn > 0.8f);
        assert(d / 4000 < 0.5f);                 // steps are small next to the range: it drifts, it doesn't jump
    }
    printf("ok   pink\n");

    // beat: clicks every 31 frames (16 ms hops = ~121 bpm), last click 7 frames ago
    {
        static float env[512]; int ph; float cf;
        for (int i = 0; i < 512; i++) env[i] = ((511 - 7 - i) % 31 == 0) ? 1.0f : 0.02f * ((i * 7) % 5);
        int per = beat_find(env, 512, 20, 54, &cf, &ph);
        assert(per == 31 && ph == 7 && cf > 0.5f);
        uint32_t z = 99;
        for (int i = 0; i < 512; i++) { z = z * 1103515245u + 12345u; env[i] = 0.3f + ((z >> 16) & 255) / 255.0f; }   // no beat: noise
        per = beat_find(env, 512, 20, 54, &cf, &ph);
        assert(per == 0 || cf < 0.35f);
        assert(beat_find(env, 40, 20, 54, &cf, &ph) == 0);                   // too short to tell
    }
    printf("ok   beat\n");

    puts("all passed");
    return 0;
}
