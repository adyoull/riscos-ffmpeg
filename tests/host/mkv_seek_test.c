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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>
#include "reelcore.h"
#include "fake_sdl_gl.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ---- the bytes read from files ---- */
static long long bytes_read;
ssize_t __real_read(int fd, void *buf, size_t n);
ssize_t __wrap_read(int fd, void *buf, size_t n)
{
    ssize_t r = __real_read(fd, buf, n);
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
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
