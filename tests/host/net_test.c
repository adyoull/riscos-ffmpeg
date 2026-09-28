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
 *   6. reelcore_is_network.
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
#include "reelcore.h"
#include "fake_sdl_gl.h"

static int fails;
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

    /* 6. what counts as an address */
    CHECK(reelcore_is_network("https://a.b/c") && reelcore_is_network("rtmp://x/y") && reelcore_is_network("http://x"),
          "addresses not seen as such");
    CHECK(!reelcore_is_network("SDFS::Pi.$.Films.clip/mp4") && !reelcore_is_network("file:///tmp/x.mp4") &&
          !reelcore_is_network("/tmp/a://b") && !reelcore_is_network("<Obey$Dir>.x/mp4"), "files seen as addresses");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
