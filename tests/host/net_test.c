/*
 * reelcore reading from the network (the reader thread) and from two inputs
 * (a video and its sound apart, as yt-dlp gives "bestvideo+bestaudio"),
 * against tests/host/httpserve.py, under the trapping qemu:
 *   1. an address opened with REELCORE_ASYNC: OPENING, then READY once, the
 *      size known; it plays (pictures and sound) and, after a seek, shows the
 *      same picture as the file itself at that time; reelcore_net reports
 *      what was read;
 *   2. the sound from a second address: both play, and the sound is decoded;
 *      the same with two local files (read in the caller's thread);
 *   3. the HTTP headers and user agent reach the server;
 *   4. an address that isn't there: REELCORE_FAILED with the reason;
 *   5. a server that never answers: closing while opening returns at once;
 *   6. reelcore_is_network;
 *   7. subtitles from an address; 8. a live HLS stream; 9. the sound's own
 *      address cut half way;
 *  10. a live stream as two playlists, the sound's starting 6 s after the
 *      picture's: the picture starts where the sound does;
 *  11. the sound and picture apart with every picture thrown away for 40 s
 *      (as when all are late): the reader keeps reading by the clock, so
 *      the sound doesn't run dry;
 *  12. a live stream whose playlist can't be reloaded for a while: opened
 *      again, rather than its end.
 *
 *   net_test BASE_URL SILENT_URL SAMPLES_DIR HEADER_LOG
 *   (BASE_URL serves SAMPLES_DIR, which has long_h264_aac_322_184.mp4 and
 *    the video-only and sound-only files run.sh makes from it)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <libavcodec/avcodec.h>
#include "reelcore.h"
#include "fake_sdl_gl.h"

static int fails;

/* case 11: pictures decoded and thrown away (a hardware decoder's
   drop_before, every picture late) */
static int drop_pictures;
int __real_avcodec_receive_frame(AVCodecContext *c, AVFrame *f);
int __wrap_avcodec_receive_frame(AVCodecContext *c, AVFrame *f)
{
    int r = __real_avcodec_receive_frame(c, f);
    while (r >= 0 && drop_pictures && c->codec_type == AVMEDIA_TYPE_VIDEO) {
        av_frame_unref(f);
        r = __real_avcodec_receive_frame(c, f);
    }
    return r;
}
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static double real_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* updates (fake time 10 ms a step, real time too, so the reader can read)
   until the result is want (or a picture, want < 0); returns the result */
static int pump(ReelCore *v, int want, double real_max, int *pictures)
{
    double end = real_now() + real_max;
    int r = -1;
    while (real_now() < end) {
        r = reelcore_update(v);
        if (r == REELCORE_NEW_FRAME && pictures)
            (*pictures)++;
        if (r == want || r == REELCORE_FAILED || (want < 0 && r == REELCORE_NEW_FRAME))
            return r;
        fake_time += 0.01;
        usleep(2000);
    }
    return r;
}

static uint32_t *picture(ReelCore *v, int w, int h)
{
    uint32_t *p = malloc((size_t)w * h * 4);
    reelcore_draw_pixels(v, p, w * 4, w, h, 0, REELCORE_STRETCH);
    return p;
}

int main(int argc, char **argv)
{
    const char *base = argv[1], *silent = argv[2], *dir = argv[3], *hlog = argv[4];
    char url[512], aurl[512], file[512], vfile[512], afile[512];
    ReelCoreSource src;
    ReelCoreNet ns;
    ReelCore *v, *f;
    int r, pics = 0, opening = 0;

    snprintf(url, sizeof(url), "%s/long_h264_aac_322_184.mp4", base);
    snprintf(file, sizeof(file), "%s/long_h264_aac_322_184.mp4", dir);

    /* 1. async: OPENING, READY, plays; a seek shows the file's picture */
    memset(&src, 0, sizeof(src));
    src.url = url;
    src.title = "A test";
    v = reelcore_open_source(&src, REELCORE_ASYNC);
    CHECK(v != NULL, "async open returned NULL");
    if (!v)
        return 1;
    CHECK(reelcore_width(v) == 0, "size known before it was open");
    {
        double end = real_now() + 20;
        while (real_now() < end && (r = reelcore_update(v)) == REELCORE_OPENING) {
            opening++;
            fake_time += 0.001;            /* (FFmpeg's own waits follow the clock) */
            usleep(1000);
        }
    }
    CHECK(r == REELCORE_READY, "async open: %d (want READY), %s", r, reelcore_last_error());
    printf("  opened in %d updates (OPENING, then READY)\n", opening);
    CHECK(reelcore_width(v) == 322 && reelcore_height(v) == 184, "size %dx%d", reelcore_width(v), reelcore_height(v));
    CHECK(reelcore_update(v) != REELCORE_READY, "READY twice");
    {
        double q0 = fake_queued_total, p0;
        pump(v, -1, 10, &pics);
        p0 = reelcore_position(v);
        for (double end = fake_time + 2.0; fake_time < end; ) {
            if (reelcore_update(v) == REELCORE_NEW_FRAME)
                pics++;
            fake_time += 0.01;
            usleep(500);
        }
        printf("  played: %d pictures, %.2f s, %.2f s of sound queued\n", pics, reelcore_position(v) - p0,
               (fake_queued_total - q0) / (48000 * 4.0));
        CHECK(pics >= 30, "only %d pictures in 2 s", pics);
        CHECK(fake_queued_total - q0 > 48000 * 4.0, "only %.2f s of sound", (fake_queued_total - q0) / (48000 * 4.0));
    }
    {
        char info[4096];
        reelcore_media_info(v, info, sizeof(info));
        CHECK(strstr(info, "Title\tA test") && strstr(info, "Address\thttp://"), "media info: %.200s", info);
    }
    r = reelcore_seek(v, 4.0);
    CHECK(r == 0, "seek: %d", r);
    r = pump(v, -1, 20, NULL);
    CHECK(r == REELCORE_NEW_FRAME && fabs(reelcore_position(v) - 4.0) < 0.1, "after the seek: %d at %.2f",
          r, reelcore_position(v));
    f = reelcore_open(file, REELCORE_NO_AUDIO);
    if (f) {
        reelcore_seek(f, 4.0);
        pump(f, -1, 10, NULL);
        {
            uint32_t *a = picture(v, 322, 184), *b = picture(f, 322, 184);
            CHECK(fabs(reelcore_position(v) - reelcore_position(f)) < 0.001 && !memcmp(a, b, 322 * 184 * 4),
                  "after the seek, the picture at %.2f isn't the file's at %.2f", reelcore_position(v),
                  reelcore_position(f));
            free(a); free(b);
        }
        reelcore_close(f);
    }
    reelcore_net(v, &ns);
    printf("  network: %.1f s read ahead, %u KB queued, %lld KB read%s\n", ns.ahead, ns.bytes_ahead >> 10,
           ns.bytes_read >> 10, ns.ended ? ", all" : "");
    CHECK(ns.bytes_read > 100000 && !ns.opening && !ns.error[0], "net: %lld bytes, opening %d, error '%s'",
          ns.bytes_read, ns.opening, ns.error);
    reelcore_close(v);

    /* 2. the sound from another address; and from another file */
    for (int local = 0; local < 2; local++) {
        const char *where = local ? dir : base;
        double q0 = fake_queued_total;
        int n = 0;
        snprintf(vfile, sizeof(vfile), "%s/net_video_only.mp4", where);
        snprintf(afile, sizeof(afile), "%s/net_sound_only.m4a", where);
        memset(&src, 0, sizeof(src));
        src.url = vfile;
        src.audio_url = afile;
        v = reelcore_open_source(&src, 0);
        CHECK(v != NULL, "video + sound (%s): %s", local ? "files" : "addresses", reelcore_last_error());
        if (!v)
            continue;
        CHECK(reelcore_has_audio(v), "video + sound (%s): no sound", local ? "files" : "addresses");
        pump(v, -1, 10, &n);
        for (double end = fake_time + 2.0; fake_time < end; ) {
            if (reelcore_update(v) == REELCORE_NEW_FRAME)
                n++;
            fake_time += 0.01;
            usleep(500);
        }
        {
            ReelCoreStats st;
            reelcore_stats(v, &st);
            printf("  video + sound from %s: %d pictures, %.2f s of sound queued, the clock from the %s, at %.2f s\n",
                   local ? "two files" : "two addresses", n, st.sound_queued,
                   st.clock_source == 1 ? "sound" : "timer", reelcore_position(v));
            CHECK(n >= 30 && st.clock_source == 1 && st.sound_queued > 0,
                  "video + sound: %d pictures, clock %d, %.2f s of sound queued", n, st.clock_source, st.sound_queued);
        }
        (void)q0;
        r = reelcore_seek(v, 3.0);
        pump(v, -1, 20, NULL);
        CHECK(fabs(reelcore_position(v) - 3.0) < 0.1, "video + sound: seek to %.2f", reelcore_position(v));
        reelcore_close(v);
    }

    /* 3. headers and user agent */
    memset(&src, 0, sizeof(src));
    src.url = url;
    src.headers = "X-Reel-Test: 1\r\nReferer: https://example.com/\r\n";
    src.user_agent = "ReelTest/1.0";
    v = reelcore_open_source(&src, 0);
    CHECK(v != NULL, "open with headers: %s", reelcore_last_error());
    reelcore_close(v);
    {
        FILE *h = fopen(hlog, "r");
        char buf[8192] = "";
        size_t got = h ? fread(buf, 1, sizeof(buf) - 1, h) : 0;
        buf[got] = 0;
        if (h) fclose(h);
        CHECK(strstr(buf, "X-Reel-Test: 1") && strstr(buf, "User-Agent: ReelTest/1.0") &&
              strstr(buf, "Referer: https://example.com/"), "the server didn't get the headers");
        if (strstr(buf, "X-Reel-Test"))
            printf("  headers and user agent reached the server\n");
    }

    /* 4. not there */
    snprintf(url, sizeof(url), "%s/not_here.mp4", base);
    src.url = url;
    src.headers = src.user_agent = NULL;
    v = reelcore_open_source(&src, REELCORE_ASYNC);
    r = pump(v, REELCORE_FAILED, 20, NULL);
    CHECK(r == REELCORE_FAILED && strstr(reelcore_last_error(), "404"), "not there: %d, '%s'", r, reelcore_last_error());
    printf("  not there: %s\n", reelcore_last_error());
    reelcore_close(v);

    /* 5. a server that never answers: closing while opening doesn't wait */
    src.url = silent;
    v = reelcore_open_source(&src, REELCORE_ASYNC);
    for (int i = 0; i < 20; i++) {
        reelcore_update(v);
        fake_time += 0.01;
        usleep(10000);
    }
    CHECK(reelcore_update(v) == REELCORE_OPENING, "silent server: not still opening");
    {
        double t0 = real_now();
        reelcore_close(v);
        printf("  closed while opening in %.2f s\n", real_now() - t0);
        CHECK(real_now() - t0 < 2, "closing while opening took %.1f s", real_now() - t0);
    }

    /* 7. an address with a subtitle track shown (an MKV whose subtitles are
       the default): the reader queues subtitle packets as a third kind, and
       mustn't take them for the end of the video (they were written past
       its two-kind table, over the video's end flag: read-ahead then said
       1e9 s, and it stopped reading ahead) */
    snprintf(url, sizeof(url), "%s/net_subs.mkv", base);
    v = reelcore_open(url, REELCORE_NO_AUDIO);
    CHECK(v != NULL, "subtitled address: %s", reelcore_last_error());
    if (v) {
        int subs = reelcore_subtitle_tracks(v), spics = 0;
        if (subs > 0)
            reelcore_set_subtitle_track(v, 0);
        pump(v, -1, 10, &spics);
        for (double end = fake_time + 3.0; fake_time < end; ) {
            if (reelcore_update(v) == REELCORE_NEW_FRAME)
                spics++;
            fake_time += 0.01;
            usleep(500);
        }
        reelcore_net(v, &ns);
        printf("  with subtitles (%d track%s): %d pictures, %.1f s read ahead%s\n", subs, subs == 1 ? "" : "s", spics,
               ns.ahead, ns.ended ? ", all" : "");
        CHECK(subs >= 1 && spics >= 50 && ns.ahead >= 0 && ns.ahead < 100,
              "with subtitles: %d tracks, %d pictures, %.1f s ahead", subs, spics, ns.ahead);
        reelcore_close(v);
    }

    /* 6. what counts as an address */
    CHECK(reelcore_is_network("https://a.b/c") && reelcore_is_network("rtmp://x/y") && reelcore_is_network("http://x"),
          "addresses not seen as such");
    CHECK(!reelcore_is_network("SDFS::Pi.$.Films.clip/mp4") && !reelcore_is_network("file:///tmp/x.mp4") &&
          !reelcore_is_network("/tmp/a://b") && !reelcore_is_network("<Obey$Dir>.x/mp4"), "files seen as addresses");

    /* 8. a live HLS stream (no end; its segments 0.3 s late): started 6
       segments behind the newest (FFmpeg's own -3 gave YouTube's webcams
       too little in hand), and its first picture shown only once 4 s are
       read (on the first packet, it soon ran dry: buffering at once) */
    {
        FILE *lf = fopen(hlog, "w");             /* (this test's requests only) */
        if (lf)
            fclose(lf);
        snprintf(url, sizeof(url), "%s/live.m3u8", base);
        memset(&src, 0, sizeof(src));
        src.url = url;
        v = reelcore_open_source(&src, REELCORE_ASYNC);
        CHECK(v != NULL, "live: open");
        if (v) {
            double ahead = -1, waited = 0;
            int first = -1, held = 0;
            r = pump(v, REELCORE_READY, 20, NULL);
            CHECK(r == REELCORE_READY, "live: %d (want READY), %s", r, reelcore_last_error());
            {
                double end = real_now() + 30, t0 = fake_time;
                while (real_now() < end) {
                    r = reelcore_update(v);
                    if (r == REELCORE_NEW_FRAME)
                        break;
                    reelcore_net(v, &ns);
                    held += ns.buffering;
                    fake_time += 0.01;
                    usleep(2000);
                }
                waited = fake_time - t0;
            }
            reelcore_net(v, &ns);
            ahead = ns.ahead;
            lf = fopen(hlog, "r");
            if (lf) {
                char line[300];
                while (fgets(line, sizeof(line), lf))
                    if (sscanf(line, "GET /live_slow%d.ts", &first) == 1)
                        break;
                fclose(lf);
            }
            printf("  live HLS: first segment %d of 0-11, first picture with %.1f s read, after %.1f s (buffering shown %d "
                   "times)\n", first, ahead, waited, held);
            CHECK(r == REELCORE_NEW_FRAME, "live: no picture (%d)", r);
            CHECK(first == 6, "live: started at segment %d (want 6: six behind the newest, 11)", first);
            CHECK(ahead >= 3.9 && held > 0, "live: first picture with only %.1f s read (want 4), buffering shown %d times",
                  ahead, held);
            reelcore_close(v);
        }
    }

    /* 9. the sound from an address of its own whose connection is cut half
       way, its server then away for longer than FFmpeg's own reconnecting
       tries (TubeSearch's YouTube videos, picture and sound apart): it's
       opened again (waiting longer each try) and goes on from where it got
       to, and the sound plays to the end (before: the picture went on
       alone) */
    {
        FILE *lf = fopen(hlog, "w");
        int gets = 0, n = 0;
        double q0 = fake_queued_total, dur;
        if (lf)
            fclose(lf);
        snprintf(vfile, sizeof(vfile), "%s/net_video_only.mp4", base);
        snprintf(afile, sizeof(afile), "%s/net_sound_cutonce.m4a", base);
        memset(&src, 0, sizeof(src));
        src.url = vfile;
        src.audio_url = afile;
        v = reelcore_open_source(&src, 0);
        CHECK(v != NULL, "sound cut half way: %s", reelcore_last_error());
        if (v) {
            ReelCoreStats st;
            dur = reelcore_duration(v);
            r = pump(v, REELCORE_END, 90, &n);
            reelcore_stats(v, &st);
            reelcore_net(v, &ns);
            lf = fopen(hlog, "r");
            if (lf) {
                char line[300];
                while (fgets(line, sizeof(line), lf))
                    gets += !strncmp(line, "GET /net_sound_cutonce.m4a", 26);
                fclose(lf);
            }
            printf("  sound cut half way: %d requests for it, %.2f s of %.2f s of sound, %d pictures, end %d%s%s\n",
                   gets, (fake_queued_total - q0) / (48000 * 4.0), dur, n, r == REELCORE_END,
                   ns.error[0] ? ", " : "", ns.error);
            CHECK(r == REELCORE_END && gets >= 2 && reelcore_has_audio(v) &&
                  (fake_queued_total - q0) / (48000 * 4.0) > dur - 0.5,
                  "sound cut half way: end %d, %d requests, %.2f s of %.2f s of sound", r == REELCORE_END, gets,
                  (fake_queued_total - q0) / (48000 * 4.0), dur);
            reelcore_close(v);
        }
    }

    /* 10. a live stream as two playlists (TubeSearch's YouTube live: the
       picture's variant and the sound's AUDIO playlist), each started 6
       segments back: the sound's segments are shorter, so it starts 6 s
       after the picture. Before, the picture started 6 s behind the sound
       and stayed behind (on a CM4, every picture given back late) */
    snprintf(vfile, sizeof(vfile), "%s/livev.m3u8", base);
    snprintf(afile, sizeof(afile), "%s/livea.m3u8", base);
    memset(&src, 0, sizeof(src));
    src.url = vfile;
    src.audio_url = afile;
    v = reelcore_open_source(&src, REELCORE_ASYNC);
    CHECK(v != NULL, "live, two playlists: open");
    if (v) {
        double first = -1;
        unsigned late0;
        ReelCoreStats st;
        r = pump(v, REELCORE_READY, 20, NULL);
        CHECK(r == REELCORE_READY, "live, two playlists: %d (want READY), %s", r, reelcore_last_error());
        r = pump(v, -1, 30, NULL);
        if (r == REELCORE_NEW_FRAME)
            first = reelcore_position(v);
        reelcore_stats(v, &st);
        late0 = st.late;
        for (double end = fake_time + 2.0; fake_time < end; ) {
            reelcore_update(v);
            fake_time += 0.01;
            usleep(1000);
        }
        reelcore_stats(v, &st);
        printf("  live, two playlists (the sound 6 s later): first picture at %.2f s, the clock from the %s, "
               "%u pictures late in the 2 s after\n", first, st.clock_source == 1 ? "sound" : "timer", st.late - late0);
        CHECK(first >= 4.9 && first <= 7.6, "live, two playlists: the first picture at %.2f s (want 5-7.5: where the sound "
              "starts)", first);
        CHECK(st.clock_source == 1 && st.late - late0 < 5, "live, two playlists: clock %d, %u pictures late",
              st.clock_source, st.late - late0);
        reelcore_close(v);
    }

    /* 11. picture and sound apart, every picture thrown away for 40 s: the
       reader goes by the clock (the sound playing on), not the last
       picture shown, so it keeps reading and the sound never runs dry */
    snprintf(vfile, sizeof(vfile), "%s/long_video_only.mp4", base);
    snprintf(afile, sizeof(afile), "%s/long_sound_only.m4a", base);
    memset(&src, 0, sizeof(src));
    src.url = vfile;
    src.audio_url = afile;
    v = reelcore_open_source(&src, 0);
    CHECK(v != NULL, "no pictures for 40 s: open: %s", reelcore_last_error());
    if (v) {
        ReelCoreStats st;
        double t0, dry = 0, p0, pos0, from = 0;
        pump(v, -1, 10, NULL);
        reelcore_stats(v, &st);
        p0 = st.clock;
        pos0 = reelcore_position(v);
        drop_pictures = 1;
        t0 = fake_time;
        while (fake_time < t0 + 40) {
            reelcore_update(v);
            reelcore_stats(v, &st);
            if (fake_time > t0 + 1 && st.sound_queued <= 0)
                dry += 0.01;
            if (fake_time < t0 + 20) {
                reelcore_net(v, &ns);
                from = ns.from;
            }
            fake_time += 0.01;
            usleep(1500);
        }
        drop_pictures = 0;
        reelcore_stats(v, &st);
        reelcore_net(v, &ns);
        printf("  no pictures for 40 s: the clock went %.1f s on, the sound ran dry for %.1f s; 20 s in, the reader "
               "counted from %.1f s (the picture at %.1f s)\n", st.clock - p0, dry, from - pos0, st.position - pos0);
        CHECK(st.clock - p0 > 38 && dry < 0.5 && reelcore_has_audio(v),
              "no pictures for 40 s: the clock %.1f s on, the sound dry for %.1f s", st.clock - p0, dry);
        CHECK(from - pos0 > 19 && from - pos0 < 21, "no pictures for 40 s: 20 s in, the reader counted from %.1f s "
              "(want the clock's 20 s, not the picture's %.1f s)", from - pos0, st.position - pos0);
        reelcore_close(v);
    }

    /* 12. a live stream (picture and sound in one playlist) whose playlist
       can't be reloaded for a while (AcornSSL's connection dropping, a CM4
       after 28 minutes of a YouTube live stream): opened again, waiting
       longer each try, rather than taken for the end */
    snprintf(url, sizeof(url), "%s/live_failmid.m3u8", base);
    memset(&src, 0, sizeof(src));
    src.url = url;
    v = reelcore_open_source(&src, REELCORE_ASYNC);
    CHECK(v != NULL, "live, reload failing: open");
    if (v) {
        int pics = 0;
        r = pump(v, REELCORE_READY, 20, NULL);
        CHECK(r == REELCORE_READY, "live, reload failing: %d (want READY), %s", r, reelcore_last_error());
        for (double end = real_now() + 25; real_now() < end; ) {
            if (reelcore_update(v) == REELCORE_NEW_FRAME)
                pics++;
            fake_time += 0.01;
            usleep(1500);
        }
        reelcore_net(v, &ns);
        printf("  live, its playlist failing for a while: %d pictures, %s%s%s\n", pics,
               ns.ended ? "taken for the end" : "still open", ns.error[0] ? ", " : "", ns.error);
        CHECK(!ns.ended && !ns.error[0] && pics >= 100, "live, reload failing: ended %d, '%s', %d pictures", ns.ended,
              ns.error, pics);
        reelcore_close(v);
    }

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
