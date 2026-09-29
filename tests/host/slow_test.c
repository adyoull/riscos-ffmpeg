/*
 * ffegl on a CPU too slow for the video (as a Pi with 1280x544 H.264):
 * decoding a video packet costs 0.06 s of (fake) time (avcodec_send_packet
 * is wrapped), so about 16 pictures a second for a 25 fps clip; a skipped
 * non-reference frame costs a tenth of that (about every other packet
 * here), and with keyframes only, only keyframes cost. The sound (fake SDL
 * device, the clock) keeps going; ffegl must fall back to skipping
 * non-reference frames (or decoding only keyframes) and keep the pictures
 * near the clock, and the clip must still end on time. The sound queued
 * must stay near reelcore's own limit however far the video reads ahead
 * (a 4K film on a Pi: 15 s queued, StreamManager refused it, the sound
 * was given up).
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
static double cost = 0.06;                   /* a decoded picture (heavy: 0.4, a 4K film on a Pi) */
int __wrap_avcodec_send_packet(AVCodecContext *c, const AVPacket *p)
{
    static int n;
    if (c->codec_type == AVMEDIA_TYPE_VIDEO && p) {
        int key = p->flags & AV_PKT_FLAG_KEY;
        n++;
        if (c->skip_frame == AVDISCARD_DEFAULT || key)
            fake_time += cost;
        else if (c->skip_frame == AVDISCARD_NONREF)
            fake_time += (n & 1) ? cost : cost / 10;
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
    double worst = 0, end_at = 0, most_sound = 0;
    char d[400];
    reelcore_set_log(log_line, 1);
    v = reelcore_open(argv[1], 0);
    CHECK(v && reelcore_has_audio(v), "open with sound");
    if (!v)
        return 1;
    for (int i = 0; i < 20000 && r != REELCORE_END; i++) {
        r = reelcore_update(v);
        {
            ReelCoreStats st;
            reelcore_stats(v, &st);
            if (st.sound_queued > most_sound)
                most_sound = st.sound_queued;
        }
        if (r == REELCORE_NEW_FRAME) {
            double lag = fake_time - reelcore_position(v);
            frames++;
            if (fake_time > 1.5 && lag > worst) { worst = lag; worst_at = i; }
        }
        fake_time += 0.002;                  /* the rest of the loop */
        end_at = fake_time;
    }
    reelcore_debug(v, d, sizeof(d));
    printf("  %d pictures shown, ended at %.1f s, worst lag %.2f s (update %d), most sound queued %.2f s\n  %s\n",
           frames, end_at, worst, worst_at, most_sound, d);
    CHECK(most_sound < 0.6, "the sound queued reached %.2f s", most_sound);
    CHECK(r == REELCORE_END && end_at < 7.5, "didn't end on time: %.1f s", end_at);
    CHECK(strstr(d, " 0 skip spells") == NULL, "never skipped frames");
    CHECK(worst < 2.0, "pictures fell %.2f s behind", worst);
    CHECK(frames > 12, "only %d pictures", frames);
    reelcore_close(v);

    /* heavy: key frames every second, each 0.4 s to decode, so it falls to
       key frames only and reads ahead to find them; the sound it reads on
       the way must wait as packets, not be queued (the 4K film: 15 s) */
    if (argc > 2) {
        double t0;
        cost = 0.4;
        most_sound = 0;
        v = reelcore_open(argv[2], 0);
        CHECK(v && reelcore_has_audio(v), "heavy: open with sound");
        if (!v)
            return 1;
        t0 = fake_time;
        r = 0;
        for (int i = 0; i < 20000 && r != REELCORE_END && fake_time - t0 < 8; i++) {
            ReelCoreStats st;
            r = reelcore_update(v);
            reelcore_stats(v, &st);
            if (st.sound_queued > most_sound)
                most_sound = st.sound_queued;
            fake_time += 0.002;
        }
        reelcore_debug(v, d, sizeof(d));
        printf("  heavy: most sound queued %.2f s\n  %s\n", most_sound, d);
        CHECK(most_sound < 0.6, "heavy: the sound queued reached %.2f s", most_sound);
        CHECK(strstr(d, "keyframes only") != NULL || strstr(d, "skip spells") != NULL, "heavy: didn't skip");
        reelcore_close(v);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
