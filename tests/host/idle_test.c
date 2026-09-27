/*
 * reelcore_idle_time(): sleeping until the next picture is due (Reel 0.1.9's
 * Wimp_PollIdle) must show the pictures as smoothly as polling flat out.
 * The same clip is played twice against the fake clock and fake sound:
 * once waking every 0.5 ms (flat out), once sleeping for the whole
 * centiseconds reelcore_idle_time() allows (as Reel does), with each wake-up
 * up to 1 cs late at random (a busy desktop), and decoding a picture
 * costing 8 ms. Compared: pictures shown, late ones, how late each picture
 * was shown against its time, and how many wake-ups each needed.
 *
 *   idle_test CLIP   (a 6 s clip with sound)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "reelcore.h"
#include "fake_sdl_gl.h"
#include "libavcodec/avcodec.h"

int __real_avcodec_send_packet(AVCodecContext *c, const AVPacket *p);
int __wrap_avcodec_send_packet(AVCodecContext *c, const AVPacket *p)
{
    if (c->codec_type == AVMEDIA_TYPE_VIDEO && p)
        fake_time += 0.008;
    return __real_avcodec_send_packet(c, p);
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

typedef struct { int shown, late, wakes; double worst, sum; } result_t;

static result_t run(const char *clip, int sleep)
{
    result_t res = { 0 };
    ReelCore *v = reelcore_open(clip, 0);
    double t0 = fake_time;
    int r = 0;
    srand(1);
    while (v && r != REELCORE_END && fake_time - t0 < 20) {
        r = reelcore_update(v);
        res.wakes++;
        if (r == REELCORE_NEW_FRAME && res.shown++ > 3) {
            ReelCoreStats st;
            double lateness;
            reelcore_stats(v, &st);
            lateness = st.clock - st.position;      /* shown this long after its time */
            if (lateness > res.worst) res.worst = lateness;
            res.sum += fabs(lateness);
        }
        if (sleep) {
            int cs = (int)(reelcore_idle_time(v) * 100);
            if (cs > 0)
                fake_time = floor(fake_time * 100 + cs) / 100 + (rand() % 100) / 10000.0;  /* + 0..1 cs late */
            else
                fake_time += 0.0005;
        } else
            fake_time += 0.0005;
    }
    if (v) {
        res.late = reelcore_dropped_frames(v);
        reelcore_close(v);
    }
    return res;
}

int main(int argc, char **argv)
{
    result_t busy = run(argv[1], 0), idle = run(argv[1], 1);
    printf("  flat out: %d pictures, %d late, %d wake-ups, lateness mean %.1f ms, worst %.1f ms\n",
           busy.shown, busy.late, busy.wakes, busy.sum * 1000 / busy.shown, busy.worst * 1000);
    printf("  sleeping: %d pictures, %d late, %d wake-ups, lateness mean %.1f ms, worst %.1f ms\n",
           idle.shown, idle.late, idle.wakes, idle.sum * 1000 / idle.shown, idle.worst * 1000);
    CHECK(idle.shown >= busy.shown - 1 && idle.late <= busy.late, "sleeping lost pictures");
    CHECK(idle.worst < 0.021, "a picture shown %.1f ms late", idle.worst * 1000);
    CHECK(idle.wakes * 5 < busy.wakes, "sleeping didn't save wake-ups (%d vs %d)", idle.wakes, busy.wakes);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
