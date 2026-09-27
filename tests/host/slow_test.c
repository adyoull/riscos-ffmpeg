/*
 * ffegl on a CPU too slow for the video (as a Pi with 1280x544 H.264):
 * decoding a video packet costs 0.06 s of (fake) time (avcodec_send_packet
 * is wrapped), so about 16 pictures a second for a 25 fps clip; a skipped
 * non-reference frame costs a tenth of that (about every other packet
 * here), and with keyframes only, only keyframes cost. The sound (fake SDL
 * device, the clock) keeps going; ffegl must fall back to skipping
 * non-reference frames (or decoding only keyframes) and keep the pictures
 * near the clock, and the clip must still end on time.
 *
 *   slow_test CLIP   (a 6 s clip with B-frames and sound)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "reelcore.h"
#include "fake_sdl_gl.h"
#include "libavcodec/avcodec.h"

int __real_avcodec_send_packet(AVCodecContext *c, const AVPacket *p);
int __wrap_avcodec_send_packet(AVCodecContext *c, const AVPacket *p)
{
    static int n;
    if (c->codec_type == AVMEDIA_TYPE_VIDEO && p) {
        int key = p->flags & AV_PKT_FLAG_KEY;
        n++;
        if (c->skip_frame == AVDISCARD_DEFAULT || key)
            fake_time += 0.06;
        else if (c->skip_frame == AVDISCARD_NONREF)
            fake_time += (n & 1) ? 0.06 : 0.006;
        else
            fake_time += 0.002;
    }
    return __real_avcodec_send_packet(c, p);
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void log_line(int level, const char *line)
{
    if (strstr(line, "behind"))
        printf("  %.1f s: %s\n", fake_time, line);
}

int main(int argc, char **argv)
{
    ReelCore *v;
    int r = 0, frames = 0, worst_at = 0;
    double worst = 0, end_at = 0;
    char d[400];
    reelcore_set_log(log_line, 1);
    v = reelcore_open(argv[1], 0);
    CHECK(v && reelcore_has_audio(v), "open with sound");
    if (!v)
        return 1;
    for (int i = 0; i < 20000 && r != REELCORE_END; i++) {
        r = reelcore_update(v);
        if (r == REELCORE_NEW_FRAME) {
            double lag = fake_time - reelcore_position(v);
            frames++;
            if (fake_time > 1.5 && lag > worst) { worst = lag; worst_at = i; }
        }
        fake_time += 0.002;                  /* the rest of the loop */
        end_at = fake_time;
    }
    reelcore_debug(v, d, sizeof(d));
    printf("  %d pictures shown, ended at %.1f s, worst lag %.2f s (update %d)\n  %s\n",
           frames, end_at, worst, worst_at, d);
    CHECK(r == REELCORE_END && end_at < 7.5, "didn't end on time: %.1f s", end_at);
    CHECK(strstr(d, " 0 skip spells") == NULL, "never skipped frames");
    CHECK(worst < 2.0, "pictures fell %.2f s behind", worst);
    CHECK(frames > 12, "only %d pictures", frames);
    reelcore_close(v);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
