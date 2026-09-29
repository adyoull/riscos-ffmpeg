/*
 * The clock pictures are shown by (reelcore's clock_smooth: the system
 * timer, steered by the sound). The fake sound device reports what it has
 * played only in whole blocks of 2048 sample frames (FAKE_AUDIO_BLOCK), as
 * StreamManager does: 46 ms at 44.1 kHz, longer than a 30 fps picture.
 * Against those steps Reel 0.1.18 skipped the picture before each step as
 * late (21-26 of 30 a second on a Pi 4, sample-10s-720p.mp4).
 *
 * A 30 fps, 44.1 kHz clip is played, looking every millisecond:
 *   - nearly every picture is shown, none skipped as late;
 *   - evenly: the real time between two pictures is their time apart in the
 *     file, within a few ms on average (snapping to each step gave bursts);
 *   - in sync with what is heard: the picture's time minus the sound played
 *     so far stays steady (its spread is small);
 *   - the same with the sound hardware 1% fast against the timer (the
 *     clock must follow the sound, not the timer);
 *   - and sleeping between pictures in whole centiseconds, each wake-up up
 *     to 1 cs late, as Reel's Wimp_PollIdle does.
 *
 * A 60 fps clip, with decoding taking time (6 ms a picture, and once a
 * second 8 pictures at 24 ms, slower than they're shown): reelcore keeps
 * 0.13 s of pictures decoded ahead (8 at 60 fps) and shows a picture that
 * is due before decoding more, so the slow run is ridden out and none is
 * late. With 3 ahead (all there was before) the cushion was 50 ms and the
 * slow run made pictures late; decoding several in a row to fill up
 * afterwards made the next ones late too.
 *
 *   clock_test CLIP30 CLIP60   (30 and 60 fps, 44.1 kHz sound)
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "reelcore.h"
#include "libavcodec/avcodec.h"
#include "fake_sdl_gl.h"

static int fails, load, vpackets;

/* decoding a video packet takes time (load): 6 ms, and 8 in every 60 take 24 ms */
int __real_avcodec_send_packet(AVCodecContext *c, const AVPacket *p);
int __wrap_avcodec_send_packet(AVCodecContext *c, const AVPacket *p)
{
    if (load && c->codec_type == AVMEDIA_TYPE_VIDEO && p)
        fake_time += vpackets++ % 60 < 52 ? 0.006 : 0.024;
    return __real_avcodec_send_packet(c, p);
}

static void run(const char *clip, double rate, int sleep, const char *what)
{
    ReelCore *v;
    ReelCoreStats st;
    int shown = 0, r = 0, n = 0, most = 0;
    double t0, s = 0, s2 = 0, first = 0;
    fake_audio_rate = rate;
    v = reelcore_open(clip, 0);
    if (!v) {
        printf("FAIL: %s: can't open\n", what);
        fails++;
        return;
    }
    t0 = fake_time;
    while (r != REELCORE_END && fake_time - t0 < 5.5) {
        r = reelcore_update(v);
        {
            ReelCoreStats q;
            reelcore_stats(v, &q);
            if (q.pictures_waiting > most)
                most = q.pictures_waiting;
        }
        if (r == REELCORE_NEW_FRAME && ++shown > 15) {       /* (after half a second) */
            double d = reelcore_position(v) - fake_audio_seconds();
            if (!n)
                first = d;
            d -= first;
            s += d;
            s2 += d * d;
            n++;
        }
        if (sleep) {                            /* as Reel: Wimp_PollIdle in whole cs, woken up to 1 cs late */
            int cs = (int)(reelcore_idle_time(v) * 100);
            if (cs > 0)
                fake_time = floor(fake_time * 100 + cs) / 100 + (rand() % 100) / 10000.0;
            else
                fake_time += 0.002;
        } else
            fake_time += 0.001;
    }
    reelcore_stats(v, &st);
    {
        double mean = n ? s / n : 0, sd = n ? sqrt(s2 / n - mean * mean) : 0;
        double pace = st.pace_n ? st.pace_sum / st.pace_n : 1, sync = st.sync_err_n ? st.sync_err_sum / st.sync_err_n : 1;
        printf("  %s: %d pictures in 5.5 s, %u late; picture spacing off by %.2f ms on average; "
               "picture against sound: drift %+.1f ms, spread %.1f ms; clock vs sound at each step %.1f ms; "
               "up to %d decoded ahead\n",
               what, shown, st.late, pace * 1000, mean * 1000, sd * 1000, sync * 1000, most);
        if (most != (st.fps > 50 ? 8 : 4))
            printf("FAIL: %s: up to %d pictures decoded ahead (want %d)\n", what, most, st.fps > 50 ? 8 : 4), fails++;
        if (st.clock_source != 1)
            printf("FAIL: %s: the clock isn't the sound's\n", what), fails++;
        if (st.late > 2 || shown < st.fps * 5.5 - 10)
            printf("FAIL: %s: %d pictures, %u late\n", what, shown, st.late), fails++;
        if (pace > (sleep ? 0.008 : 0.003))
            printf("FAIL: %s: pictures unevenly spaced (%.1f ms)\n", what, pace * 1000), fails++;
        if (sd > (sleep || load ? 0.008 : 0.004) || fabs(mean) > (load ? 0.017 : 0.006))   /* (load: measured from one picture, which may have waited) */
            printf("FAIL: %s: out of step with the sound (drift %.1f ms, spread %.1f ms)\n", what, mean * 1000, sd * 1000), fails++;
    }
    reelcore_close(v);
}

int main(int argc, char **argv)
{
    setenv("FAKE_AUDIO_BLOCK", "2048", 1);
    run(argv[1], 1.0, 0, "sound at the timer's rate");
    run(argv[1], 1.01, 0, "sound 1% fast");
    srand(1);
    run(argv[1], 1.01, 1, "sleeping between pictures as Reel does, sound 1% fast");
    load = 1;
    run(argv[2], 1.0, 0, "60 fps, decoding 6 ms a picture with runs of 24 ms");
    run(argv[2], 1.0, 1, "60 fps, the same, sleeping as Reel does");
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
