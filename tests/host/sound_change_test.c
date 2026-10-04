/*
 * reelcore when the sound changes part way (a DVB recording going from 5.1
 * to stereo at the adverts, or 48 to 22.05 kHz): the resampler is made
 * again, so every part plays, at the right speed. Before, it was set up
 * once from the start: 5.1 frames after stereo were read for planes they
 * didn't have, and 22.05 kHz frames played at twice the speed.
 *
 *   sound_change_test FILE SECONDS
 *   (FILE: a TS whose ADTS AAC has parts of different channels and rates
 *    one after the other; SECONDS: how long it is)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "reelcore.h"
#include "fake_sdl_gl.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(int argc, char **argv)
{
    double want = atof(argv[2]), q0 = fake_queued_total, got;
    ReelCore *v = reelcore_open(argv[1], 0);
    int r = 0;
    CHECK(v != NULL, "can't open %s", argv[1]);
    if (!v)
        return 1;
    for (int i = 0; i < 200000 && r != REELCORE_END; i++) {
        r = reelcore_update(v);
        fake_time += 0.005;
    }
    got = (fake_queued_total - q0) / fake_audio_bps();   /* (the rate the fake sound output was opened at) */
    printf("  %s: %.2f s of sound queued (want %.2f), end %d\n", argv[1], got, want, r == REELCORE_END);
    CHECK(r == REELCORE_END && got > want * 0.97 && got < want * 1.03, "%.2f s of sound for %.2f s", got, want);
    reelcore_close(v);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
