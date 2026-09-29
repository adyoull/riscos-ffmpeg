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
 * Then decoding only a little too slow (0.048 s a picture, 0.04 s apart):
 * reelcore must turn deblocking off by itself (a quarter less here) and
 * keep up without skipping; with REELCORE_NO_AUTOFAST it can't. A light
 * video never has it turned off, and one that gets lighter has it back.
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
        double k = c->skip_loop_filter == AVDISCARD_ALL ? cost * 0.75 : cost;
        if (c->skip_frame == AVDISCARD_DEFAULT || key)
            fake_time += k;
        else if (c->skip_frame == AVDISCARD_NONREF)
            fake_time += (n & 1) ? k : k / 10;
        else
            fake_time += 0.002;
    }
    return __real_avcodec_send_packet(c, p);
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void log_line(int level, const char *line)
{
    if (strstr(line, "behind") || strstr(line, "deblocking"))
        printf("  %.1f s: %s\n", fake_time, line);
}

/* plays clip for up to secs with pictures costing c (c2 from after2 s on) */
static double hiccup_at = -1;               /* a stall this far in (going full screen, say) */
static void play(const char *clip, int flags, double c, double c2, double after, double secs,
                 ReelCoreStats *st, unsigned *shown)
{
    int hiccuped = 0;
    ReelCore *v;
    double t0 = fake_time;
    int r = 0;
    cost = c;
    *shown = 0;
    v = reelcore_open(clip, flags);
    CHECK(v, "open %s", clip);
    if (!v)
        return;
    for (int i = 0; i < 40000 && r != REELCORE_END && fake_time - t0 < secs; i++) {
        if (fake_time - t0 > after)
            cost = c2;
        if (hiccup_at >= 0 && !hiccuped && fake_time - t0 > hiccup_at) {
            fake_time += 0.3;
            hiccuped = 1;
        }
        r = reelcore_update(v);
        if (r == REELCORE_NEW_FRAME)
            (*shown)++;
        fake_time += 0.002;
    }
    reelcore_stats(v, st);
    reelcore_close(v);
    printf("  cost %.3f%s: %u shown, %u late, %u skip spells, deblocking off %u time(s), now %s, %.1f ms a picture\n",
           c, flags & REELCORE_NO_AUTOFAST ? " (no auto)" : "", *shown, st->late, st->skip_spells,
           st->auto_fast_spells, st->auto_fast ? "off" : "on", st->decode_avg * 1000);
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
    if (argc > 2) {
        ReelCoreStats st;
        unsigned shown;
        /* a little too slow: deblocking off by itself, and no frames skipped */
        play(argv[2], 0, 0.048, 0.048, 99, 10, &st, &shown);
        CHECK(st.auto_fast && st.auto_fast_spells == 1, "a little slow: deblocking not turned off");
        CHECK(st.skip_spells == 0 && st.late < shown / 10, "a little slow: %u skip spells, %u late of %u",
              st.skip_spells, st.late, shown);
        /* the same without: it falls behind */
        play(argv[2], REELCORE_NO_AUTOFAST, 0.048, 0.048, 99, 10, &st, &shown);
        CHECK(st.auto_fast_spells == 0, "no auto: deblocking turned off");
        CHECK(st.skip_spells > 0 || st.late > shown / 10, "no auto: kept up anyway");
        /* light: left alone */
        play(argv[2], 0, 0.015, 0.015, 99, 10, &st, &shown);
        CHECK(st.auto_fast_spells == 0, "light: deblocking turned off");
        /* lighter after 2 s: on again (after 5 s off) */
        play(argv[2], 0, 0.048, 0.01, 2, 11, &st, &shown);
        CHECK(st.auto_fast_spells == 1 && !st.auto_fast, "lighter: deblocking not back on");
        /* decoding easily (0.8 of the time) with a stall 1 s in: not off
           for good (autofast1: a 1080p trailer full screen) */
        hiccup_at = 1;
        play(argv[2], REELCORE_LOOP, 0.032, 0.032, 99, 30, &st, &shown);
        hiccup_at = -1;
        CHECK(!st.auto_fast, "a stall: deblocking still off");
        /* just too slow with it (0.046 s, 0.0345 without): tried again now
           and then, less and less often, and not many frames skipped */
        play(argv[2], REELCORE_LOOP, 0.046, 0.046, 99, 60, &st, &shown);
        CHECK(st.auto_fast && st.auto_fast_spells >= 3 && st.auto_fast_spells <= 5,
              "too slow: deblocking off %u times, now %s", st.auto_fast_spells, st.auto_fast ? "off" : "on");
        CHECK(st.late < shown / 20, "too slow: %u late of %u", st.late, shown);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
