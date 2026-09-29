/*
 * The sound clock in steps (StreamManager reports what has been played a
 * block of 2048 sample frames at a time: 46 ms at 44.1 kHz). With a 30 fps
 * video, pictures are due every 33 ms; against a clock that jumps 46 ms at
 * a time the picture before each jump was thrown away as late: on a Pi 4,
 * 21-26 of 30 shown a second, 5-9 "late" (Reel 0.1.18, sample-10s-720p.mp4).
 * reelcore now runs the clock on between steps (clock_smooth). Here the
 * fake sound device reports played sound in 2048-frame blocks
 * (FAKE_AUDIO_BLOCK), and the clip is played polling every millisecond:
 * nearly every picture must be shown, each close to its time, and the
 * clock must never go backwards.
 *
 *   clock_test CLIP   (30 fps, 44.1 kHz sound)
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "reelcore.h"
#include "fake_sdl_gl.h"

int main(int argc, char **argv)
{
    ReelCore *v;
    ReelCoreStats st;
    int shown = 0, fails = 0, r = 0;
    double t0, worst = 0, last_clock = -1, back = 0;
    setenv("FAKE_AUDIO_BLOCK", "2048", 1);
    v = reelcore_open(argv[1], 0);
    if (!v)
        return 1;
    t0 = fake_time;
    while (r != REELCORE_END && fake_time - t0 < 5.0) {
        r = reelcore_update(v);
        reelcore_stats(v, &st);
        if (st.clock_source == 1) {
            if (last_clock >= 0 && st.clock < last_clock - 1e-9 && st.clock - last_clock < back)
                back = st.clock - last_clock;
            last_clock = st.clock;
        }
        if (r == REELCORE_NEW_FRAME && ++shown > 5 && st.clock - st.position > worst)
            worst = st.clock - st.position;
        fake_time += 0.001;
    }
    reelcore_stats(v, &st);
    printf("  %d pictures shown in %.1f s, %u late skipped, clock from the %s, worst %.1f ms after its time\n",
           shown, fake_time - t0, st.late, st.clock_source == 1 ? "sound" : "timer", worst * 1000);
    if (st.clock_source != 1)
        printf("FAIL: the clock isn't the sound's\n"), fails++;
    if (st.late > 3)
        printf("FAIL: %u pictures skipped as late (the clock's steps)\n", st.late), fails++;
    if (shown < 140)
        printf("FAIL: only %d pictures in 5 s at 30 fps\n", shown), fails++;
    if (worst > 0.034)
        printf("FAIL: a picture shown %.1f ms late\n", worst * 1000), fails++;
    if (back < 0)
        printf("FAIL: the clock went back %.1f ms\n", -back * 1000), fails++;
    reelcore_close(v);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
