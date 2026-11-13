/*
 * Seeking in a Matroska file without an index (no Cues), as from a local
 * file (Ralph's DVD rip on a LanMan98 share: a seek half way read most of
 * the file, minutes with the desktop stopped).
 *
 *   mkv_seek_test NOCUES.mkv CUES.mkv SECONDS
 *   (the same video twice, muxed without and with Cues; SECONDS long)
 *
 * Each file is opened and played a moment, then sought far forward, back,
 * near the end and near the start. After each seek:
 *   - the first picture is at the place asked for;
 *   - it's the same picture in both files;
 *   - without Cues, the file read for it is a small part of the file, a
 *     few seconds' worth (the bytes counted at read(), under FFmpeg's file
 *     protocol); before, it read from the start (or the last place read).
 * The file without Cues is made from the other by mkv_nocues.py (the Cues
 * made a Void element: the Duration kept, as Ralph's had; ffmpeg writing
 * to a pipe leaves out both). ffmpeg's Matroska clusters start with a
 * CRC-32, as many do.
 *
 * Then the file read only to half way, as Ralph's 6.6 GB film on a NAS
 * through LanMan98 was only to 2 GB: played from before there, and sought
 * past it, it ends there, and reelcore's log says where it stopped and
 * how far through the length that was. And with the file saying it's
 * twice that size (the size didn't tell Reel where reading stopped), a
 * seek to 45% still finds its place by the bytes, a little read.
 */
#define _FILE_OFFSET_BITS 64               /* (struct stat as FFmpeg's fstat fills it) */
#define _TIME_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <math.h>
#include <stdarg.h>
#include <libavutil/log.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>
#include "reelcore.h"
#include "fake_sdl_gl.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ---- the bytes read from files; READ_LIMIT > 0: none past it ---- */
static long long bytes_read, read_limit;
ssize_t __real_read(int fd, void *buf, size_t n);
ssize_t __wrap_read(int fd, void *buf, size_t n)
{
    ssize_t r;
    if (read_limit > 0) {
        off_t at = lseek(fd, 0, SEEK_CUR);
        if (at >= read_limit)
            return 0;
        if (at >= 0 && at + (off_t)n > read_limit)
            n = read_limit - at;
    }
    r = __real_read(fd, buf, n);
    if (r > 0)
        bytes_read += r;
    return r;
}

#define SEEKS 4
static const double where[SEEKS] = { 0.6, 0.3, 0.9, 0.1 };    /* of the length */

/* Plays PATH, seeking to each place: the first picture's position and an
   MD5 of it, and the bytes read for it */
static void play(const char *path, double seconds, double pos[SEEKS], char md5[SEEKS][33], long long bytes[SEEKS])
{
    ReelCore *v = reelcore_open(path, REELCORE_NO_AUDIO);
    uint8_t *pic = malloc(160 * 90 * 4);
    int r = 0;
    CHECK(v != NULL, "can't open %s", path);
    if (!v || !pic)
        return;
    for (int i = 0; i < 200 && reelcore_position(v) < 1; i++) {   /* a moment's play first */
        r = reelcore_update(v);
        fake_time += 0.02;
    }
    for (int k = 0; k < SEEKS; k++) {
        long long b0 = bytes_read;
        uint8_t sum[16];
        pos[k] = -1;
        md5[k][0] = 0;
        CHECK(reelcore_seek(v, where[k] * seconds) == 0, "%s: the seek to %.1f s failed", path, where[k] * seconds);
        for (int i = 0; i < 2000 && (r = reelcore_update(v)) != REELCORE_NEW_FRAME && r != REELCORE_END; i++)
            fake_time += 0.02;
        bytes[k] = bytes_read - b0;
        if (r != REELCORE_NEW_FRAME)
            continue;
        pos[k] = reelcore_position(v);
        memset(pic, 0, 160 * 90 * 4);
        reelcore_draw_pixels(v, pic, 160 * 4, 160, 90, 0, REELCORE_STRETCH);
        av_md5_sum(sum, pic, 160 * 90 * 4);
        for (int i = 0; i < 16; i++)
            sprintf(md5[k] + 2 * i, "%02x", sum[i]);
    }
    reelcore_close(v);
    free(pic);
}

/* ---- the size fstat gives, times SIZE_SCALE (FFmpeg's file protocol asks it) ---- */
static int size_scale = 1;
int __real___fstat64_time64(int fd, struct stat *st);
int __wrap___fstat64_time64(int fd, struct stat *st)
{
    int r = __real___fstat64_time64(fd, st);
    if (!r && S_ISREG(st->st_mode))
        st->st_size *= size_scale;
    return r;
}

/* ---- reelcore's "the file ended at" line, caught ---- */
static char ended_line[512];
static void log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
    char line[512];
    va_list copy;
    va_copy(copy, vl);
    av_log_default_callback(avcl, level, fmt, copy);   /* (shown as before) */
    va_end(copy);
    if (level > AV_LOG_WARNING)
        return;
    vsnprintf(line, sizeof(line), fmt, vl);
    if (strstr(line, "the file ended at"))
        snprintf(ended_line, sizeof(ended_line), "%s", line);
}

/* PATH read only to half way: played from FROM (or sought there) until it
   ends; where, and the line logged */
static double cut_short(const char *path, double from, double *logged_at, long long *logged_byte)
{
    ReelCore *v = reelcore_open(path, REELCORE_NO_AUDIO);
    double pos = -1;
    int r = 0;
    ended_line[0] = 0;
    *logged_at = -1;
    *logged_byte = -1;
    if (!v)
        return -1;
    reelcore_seek(v, from);
    for (int i = 0; i < 40000 && r != REELCORE_END; i++) {
        r = reelcore_update(v);
        if (r == REELCORE_NEW_FRAME)
            pos = reelcore_position(v);
        fake_time += 0.02;
    }
    reelcore_close(v);
    if (ended_line[0]) {
        const char *b = strstr(ended_line, "after ");
        sscanf(ended_line, "reelcore: the file ended at %lf", logged_at);
        if (b)
            sscanf(b, "after %lld", logged_byte);
    }
    return r == REELCORE_END ? pos : -2;
}

int main(int argc, char **argv)
{
    double seconds, pos[2][SEEKS];
    char md5[2][SEEKS][33];
    long long bytes[2][SEEKS];
    struct stat sb;
    if (argc != 4 || stat(argv[1], &sb) < 0) {
        fprintf(stderr, "usage: mkv_seek_test NOCUES.mkv CUES.mkv SECONDS\n");
        return 2;
    }
    seconds = atof(argv[3]);
    for (int f = 0; f < 2; f++)
        play(argv[1 + f], seconds, pos[f], md5[f], bytes[f]);
    for (int k = 0; k < SEEKS; k++) {
        double want = where[k] * seconds;
        printf("  to %6.1f s: without Cues at %6.2f s, %5.1f MB read (%4.1f%%); with Cues at %6.2f s, %5.1f MB\n",
               want, pos[0][k], bytes[0][k] / 1e6, 100.0 * bytes[0][k] / sb.st_size, pos[1][k], bytes[1][k] / 1e6);
        for (int f = 0; f < 2; f++)
            CHECK(pos[f][k] >= want - 0.05 && pos[f][k] < want + 1, "%s Cues: the seek to %.1f s showed %.2f s",
                  f ? "with" : "without", want, pos[f][k]);
        CHECK(!strcmp(md5[0][k], md5[1][k]), "the seek to %.1f s: not the same picture with and without Cues", want);
        CHECK(bytes[0][k] < sb.st_size / 25, "without Cues: the seek to %.1f s read %.1f MB of %.1f MB", want,
              bytes[0][k] / 1e6, sb.st_size / 1e6);
    }
    {
        double at, logged;
        long long byte;
        av_log_set_callback(log_cb);
        read_limit = sb.st_size / 2;
        for (int k = 0; k < 2; k++) {
            double from = k ? 0.6 * seconds : 0.4 * seconds;   /* before half way, and past it */
            double end = cut_short(argv[1], from, &logged, &byte);
            printf("  read only to %.1f MB, from %5.1f s: %s at %.2f s; logged: ended at %.0f s, after %lld bytes\n",
                   read_limit / 1e6, from, end == -2 ? "never ended" : "ended", end, logged, byte);
            CHECK(end != -2, "read short, from %.1f s: never ended", from);
            CHECK(logged >= 0, "read short, from %.1f s: no \"the file ended at\" line", from);
            CHECK(byte > read_limit - 65536 && byte <= read_limit,
                  "read short, from %.1f s: logged %lld bytes, the reads stopped at %lld", from, byte, read_limit);
            at = k ? from : end;
            CHECK(logged > 0 && fabs(logged - at) < 2, "read short, from %.1f s: logged %.0f s, not %.0f s", from, logged, at);
        }
        {   /* the size twice what can be read: a seek inside what can */
            ReelCore *v;
            char m[SEEKS][33];
            double p[SEEKS];
            long long b[SEEKS];
            (void)m; (void)p; (void)b;
            size_scale = 2;
            if ((v = reelcore_open(argv[1], REELCORE_NO_AUDIO))) {
                long long b0 = bytes_read;
                double want = 0.45 * seconds, got = -1;
                int r = 0;
                reelcore_seek(v, want);
                for (int i = 0; i < 4000 && (r = reelcore_update(v)) != REELCORE_NEW_FRAME && r != REELCORE_END; i++)
                    fake_time += 0.02;
                if (r == REELCORE_NEW_FRAME)
                    got = reelcore_position(v);
                printf("  read only to %.1f MB, the size given as %.1f MB, to %.1f s: at %.2f s, %.1f MB read\n",
                       read_limit / 1e6, 2 * sb.st_size / 1e6, want, got, (bytes_read - b0) / 1e6);
                CHECK(got >= want - 0.05 && got < want + 1, "the size wrong: the seek to %.1f s showed %.2f s", want, got);
                CHECK(bytes_read - b0 < sb.st_size / 10, "the size wrong: the seek read %.1f MB", (bytes_read - b0) / 1e6);
                reelcore_close(v);
            } else
                CHECK(0, "the size wrong: can't open %s", argv[1]);
            size_scale = 1;
        }
        read_limit = 0;
        av_log_set_callback(av_log_default_callback);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
