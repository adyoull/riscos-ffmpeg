/*
 * reelcore_open_io: playing a stream read through the caller's callbacks,
 * as a web browser hands over what a page's Media Source Extensions give
 * it (Strada, the Web Browser project's handoff of 7 November 2026).
 *
 *   io_test SRC DASHDIR
 *   SRC: a 20 s H.264 + AAC MP4 (keyframes every second);
 *   DASHDIR: the same cut by FFmpeg's DASH muxer into what MSE carries:
 *     init-0.m4s + chunk-0-NNNNN.m4s (the video, 2 s each) and
 *     init-1.m4s + chunk-1-NNNNN.m4s (the sound).
 *
 *   1. Video and sound from two streams, the segments arriving a second
 *      apart, with nothing for 2 s in the middle: plays to the end with
 *      its sound, says it's buffering meanwhile, and the reads that found
 *      nothing yet were waited out.
 *   2. A ReelCore opened at a later segment (10 s), as a browser does after
 *      a seek: its times count from there (reelcore_start_time 10), a seek
 *      forward to 2.3 s shows the same picture as SRC sought to 12.3 s, and
 *      a seek back is refused (it can only read forward).
 *   3. A seekable stream (all of SRC, with a seek callback): seeks forward
 *      and back show the same pictures as SRC opened as a file.
 *   4. A stream that never gets past its first bytes: the close doesn't
 *      wait for it.
 *   5. A read that fails: the open says so.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>
#include "reelcore.h"
#include "fake_sdl_gl.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ---- a stream in memory, its pieces arriving by the test's clock ---- */
typedef struct Feed {
    pthread_mutex_t lock;
    uint8_t *data;
    int64_t size, pos;
    int64_t *piece_end;                /* where each piece ends (the first: the init segment) */
    int pieces;
    double start, every;               /* the first piece at START, then one every EVERY s ... */
    double stall_from, stall_to;       /* ... none arriving in between */
    int ended;                         /* 1: after the last piece, the end (0); 0: AGAIN for ever */
    int fail;                          /* read gives -5 */
    int seekable;
    int agains;                        /* reads that found nothing yet */
} Feed;

static double feed_now;                /* the test's clock, as the reader thread sees it */
static pthread_mutex_t now_lock = PTHREAD_MUTEX_INITIALIZER;

static void set_now(double t)
{
    pthread_mutex_lock(&now_lock);
    feed_now = t;
    pthread_mutex_unlock(&now_lock);
}

static double get_now(void)
{
    double t;
    pthread_mutex_lock(&now_lock);
    t = feed_now;
    pthread_mutex_unlock(&now_lock);
    return t;
}

/* Bytes arrived by now */
static int64_t arrived(Feed *f)
{
    double t = get_now();
    int n = 0;
    for (int i = 0; i < f->pieces; i++) {
        double at = f->start + i * f->every;
        if (at >= f->stall_from)
            at += f->stall_to - f->stall_from;
        if (t >= at)
            n = i + 1;
    }
    return n ? f->piece_end[n - 1] : 0;
}

static int feed_read(void *opaque, uint8_t *buf, int size)
{
    Feed *f = opaque;
    int64_t have;
    int n;
    pthread_mutex_lock(&f->lock);
    if (f->fail) {
        pthread_mutex_unlock(&f->lock);
        return -5;
    }
    have = arrived(f) - f->pos;
    if (have <= 0) {
        int all = f->pos >= f->size && f->ended;
        if (!all)
            f->agains++;
        pthread_mutex_unlock(&f->lock);
        return all ? 0 : REELCORE_IO_AGAIN;
    }
    n = have < size ? (int)have : size;
    memcpy(buf, f->data + f->pos, n);
    f->pos += n;
    pthread_mutex_unlock(&f->lock);
    return n;
}

static int64_t feed_seek(void *opaque, int64_t off, int whence)
{
    Feed *f = opaque;
    int64_t to;
    pthread_mutex_lock(&f->lock);
    to = whence == REELCORE_SEEK_SIZE ? f->size : whence == SEEK_SET ? off : whence == SEEK_CUR ? f->pos + off :
         whence == SEEK_END ? f->size + off : -1;
    if (whence != REELCORE_SEEK_SIZE && to >= 0 && to <= f->size)
        f->pos = to;
    else if (whence != REELCORE_SEEK_SIZE)
        to = -1;
    pthread_mutex_unlock(&f->lock);
    return to;
}

static void add_file(Feed *f, const char *path)
{
    FILE *fp = fopen(path, "rb");
    long n;
    if (!fp) {
        printf("FAIL: no %s\n", path);
        exit(1);
    }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f->data = realloc(f->data, f->size + n);
    if (fread(f->data + f->size, 1, n, fp) != (size_t)n)
        exit(1);
    fclose(fp);
    f->size += n;
    f->piece_end = realloc(f->piece_end, (f->pieces + 1) * sizeof(*f->piece_end));
    f->piece_end[f->pieces++] = f->size;
}

/* DASHDIR's stream REP (0 video, 1 sound): its init segment and its
   chunks from number FIRST (1: the start) */
static void dash_feed(Feed *f, const char *dir, int rep, int first)
{
    char path[1024];
    memset(f, 0, sizeof(*f));
    pthread_mutex_init(&f->lock, NULL);
    snprintf(path, sizeof(path), "%s/init-%d.m4s", dir, rep);
    add_file(f, path);
    for (int i = first;; i++) {
        FILE *fp;
        snprintf(path, sizeof(path), "%s/chunk-%d-%05d.m4s", dir, rep, i);
        if (!(fp = fopen(path, "rb")))
            break;
        fclose(fp);
        add_file(f, path);
    }
    f->ended = 1;
    f->stall_from = f->stall_to = 1e9;
}

static void feed_free(Feed *f)
{
    free(f->data);
    free(f->piece_end);
    pthread_mutex_destroy(&f->lock);
}

static ReelCoreIO io_of(Feed *f, const char *format)
{
    ReelCoreIO io = { f, feed_read, f->seekable ? feed_seek : NULL, format, "test stream" };
    return io;
}

/* ---- playing ---- */

/* Runs reelcore_update until it isn't OPENING; the result */
static int wait_open(ReelCore *v)
{
    int r = REELCORE_OPENING;
    for (int i = 0; i < 3000 && r == REELCORE_OPENING; i++) {
        struct timespec ts = { 0, 2000000 };
        r = reelcore_update(v);
        fake_time += 0.002;
        set_now(fake_time);
        nanosleep(&ts, NULL);              /* (the reader thread runs meanwhile) */
    }
    return r;
}

/* The next picture: its position and an MD5 of it drawn at 160x90; -1 if none */
static double next_picture(ReelCore *v, char md5[33])
{
    static uint8_t pic[160 * 90 * 4];
    uint8_t sum[16];
    int r = 0;
    for (int i = 0; i < 4000 && (r = reelcore_update(v)) != REELCORE_NEW_FRAME && r != REELCORE_END; i++) {
        struct timespec ts = { 0, 1000000 };
        fake_time += 0.001;
        set_now(fake_time);
        nanosleep(&ts, NULL);
    }
    if (r != REELCORE_NEW_FRAME)
        return -1;
    memset(pic, 0, sizeof(pic));
    reelcore_draw_pixels(v, pic, 160 * 4, 160, 90, 0, REELCORE_STRETCH);
    av_md5_sum(sum, pic, sizeof(pic));
    for (int i = 0; i < 16; i++)
        sprintf(md5 + 2 * i, "%02x", sum[i]);
    return reelcore_position(v);
}

/* SRC opened as a file, sought to T: the picture there */
static double file_picture(const char *src, double t, char md5[33])
{
    ReelCore *v = reelcore_open(src, REELCORE_NO_AUDIO);
    double p;
    if (!v)
        return -1;
    reelcore_seek(v, t);
    p = next_picture(v, md5);
    reelcore_close(v);
    return p;
}

static void arriving(const char *dir)
{
    Feed fv, fa;
    ReelCoreIO iv, ia;
    ReelCore *v;
    ReelCoreNet ns;
    int r = 0, pictures = 0, buffering = 0, sound = 0;
    double t0 = fake_time;
    dash_feed(&fv, dir, 0, 1);
    dash_feed(&fa, dir, 1, 1);
    fv.start = fa.start = t0;              /* the init segments now, then a segment a second */
    fv.every = fa.every = 1.0;
    fv.stall_from = fa.stall_from = t0 + 3.5;   /* none for 2 s */
    fv.stall_to = fa.stall_to = t0 + 5.5;
    set_now(fake_time);
    iv = io_of(&fv, "mp4");
    ia = io_of(&fa, NULL);                 /* (told from the data) */
    v = reelcore_open_io(&iv, &ia, REELCORE_ASYNC);
    CHECK(v != NULL, "1: can't open");
    if (!v)
        return;
    reelcore_set_audio_lead(v, 1.5);
    r = wait_open(v);
    CHECK(r == REELCORE_READY, "1: opening gave %d, not READY (%s)", r, reelcore_last_error());
    for (int i = 0; i < 60000 && r != REELCORE_END && r >= 0; i++) {
        struct timespec ts = { 0, 200000 };
        r = reelcore_update(v);
        if (r == REELCORE_NEW_FRAME)
            pictures++;
        reelcore_net(v, &ns);
        if (ns.buffering && fake_time - t0 > 4)
            buffering = 1;
        if (reelcore_has_audio(v))
            sound = 1;
        fake_time += 0.002;
        set_now(fake_time);
        nanosleep(&ts, NULL);
    }
    printf("  1. two streams arriving a segment a second, none for 2 s: %d pictures, end %d after %.1f s, "
           "buffering %d, sound %d, reads that found nothing yet %d + %d\n",
           pictures, r == REELCORE_END, fake_time - t0, buffering, sound, fv.agains, fa.agains);
    CHECK(r == REELCORE_END, "1: didn't play to the end (%d)", r);
    CHECK(pictures >= 470, "1: only %d pictures of 500", pictures);
    CHECK(sound, "1: no sound");
    CHECK(buffering, "1: never said it was buffering in the stall");
    CHECK(fv.agains > 0 && fa.agains > 0, "1: the reads never had to wait");
    CHECK(reelcore_start_time(v) < 0.1, "1: starts at %.2f s", reelcore_start_time(v));
    reelcore_close(v);
    feed_free(&fv);
    feed_free(&fa);
}

static void later_segment(const char *src, const char *dir)
{
    Feed fv, fa;
    ReelCoreIO iv, ia;
    ReelCore *v;
    char m1[33], m2[33];
    double p1, p2;
    dash_feed(&fv, dir, 0, 6);             /* chunk 6: 10 s on */
    dash_feed(&fa, dir, 1, 6);
    fv.start = fa.start = -1;              /* all there already */
    fv.every = fa.every = 0;
    set_now(fake_time);
    iv = io_of(&fv, "mp4");
    ia = io_of(&fa, "mp4");
    v = reelcore_open_io(&iv, &ia, REELCORE_ASYNC | REELCORE_NO_AUDIO);
    CHECK(v && wait_open(v) == REELCORE_READY, "2: didn't open");
    if (!v)
        return;
    CHECK(reelcore_start_time(v) > 9.9 && reelcore_start_time(v) < 10.1, "2: starts at %.2f s, not 10",
          reelcore_start_time(v));
    CHECK(reelcore_seek(v, 2.3) == 0, "2: the seek forward was refused");
    p1 = next_picture(v, m1);
    p2 = file_picture(src, 12.3, m2);
    printf("  2. opened at the 10 s segment (starts at %.2f s), sought to 2.3 s: picture at %.2f s; "
           "the file sought to 12.3 s: %.2f s, %s\n", reelcore_start_time(v), p1, p2,
           strcmp(m1, m2) ? "a different picture" : "the same picture");
    CHECK(p1 >= 2.3 - 0.001 && p1 < 2.3 + 0.041, "2: the picture after the seek is at %.2f s", p1);
    CHECK(p2 >= 0 && fabs((p1 + 10) - p2) < 0.001, "2: at %.2f s, the file's at %.2f s", p1 + 10, p2);
    CHECK(!strcmp(m1, m2), "2: not the file's picture there");
    CHECK(reelcore_seek(v, 1.0) < 0, "2: a seek back was taken, but the stream can only go forward");
    CHECK(next_picture(v, m1) > 2.3, "2: didn't play on after the refused seek");
    reelcore_close(v);
    feed_free(&fv);
    feed_free(&fa);
}

static void seekable(const char *src)
{
    Feed f;
    ReelCoreIO io;
    ReelCore *v;
    static const double to[] = { 15.0, 5.0, 17.5 };
    memset(&f, 0, sizeof(f));
    pthread_mutex_init(&f.lock, NULL);
    add_file(&f, src);
    f.start = -1;
    f.ended = 1;
    f.seekable = 1;
    f.stall_from = f.stall_to = 1e9;
    set_now(fake_time);
    io = io_of(&f, NULL);
    v = reelcore_open_io(&io, NULL, REELCORE_ASYNC | REELCORE_NO_AUDIO);
    CHECK(v && wait_open(v) == REELCORE_READY, "3: didn't open");
    if (!v)
        return;
    for (int k = 0; k < 3; k++) {
        char m1[33], m2[33];
        double p1, p2;
        CHECK(reelcore_seek(v, to[k]) == 0, "3: the seek to %.1f s was refused", to[k]);
        p1 = next_picture(v, m1);
        p2 = file_picture(src, to[k], m2);
        printf("  3. seekable, to %4.1f s: picture at %.2f s, the file's at %.2f s, %s\n", to[k], p1, p2,
               strcmp(m1, m2) ? "a different picture" : "the same picture");
        CHECK(p1 >= 0 && fabs(p1 - p2) < 0.001 && !strcmp(m1, m2), "3: to %.1f s: not the file's picture", to[k]);
    }
    reelcore_close(v);
    feed_free(&f);
}

static void close_waiting(const char *dir)
{
    Feed f;
    ReelCoreIO io;
    ReelCore *v;
    struct timespec a, b;
    dash_feed(&f, dir, 0, 1);
    f.start = 0;
    f.every = 1e9;                         /* the init segment, then nothing more */
    f.ended = 0;
    set_now(fake_time);
    io = io_of(&f, "mp4");
    v = reelcore_open_io(&io, NULL, REELCORE_ASYNC | REELCORE_NO_AUDIO);
    CHECK(v != NULL, "4: can't open");
    if (!v)
        return;
    for (int i = 0; i < 100; i++) {
        struct timespec ts = { 0, 1000000 };
        CHECK(reelcore_update(v) == REELCORE_OPENING || i, "4: opened with no media segments");
        nanosleep(&ts, NULL);
    }
    clock_gettime(CLOCK_MONOTONIC, &a);
    reelcore_close(v);
    clock_gettime(CLOCK_MONOTONIC, &b);
    {
        double s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
        printf("  4. closed while it waited for more: %.3f s, %d reads found nothing yet\n", s, f.agains);
        CHECK(s < 0.5, "4: the close took %.2f s", s);
        CHECK(f.agains > 5, "4: it wasn't waiting");
    }
    feed_free(&f);
}

static void failing(const char *dir)
{
    Feed f;
    ReelCoreIO io;
    ReelCore *v;
    int r;
    dash_feed(&f, dir, 0, 1);
    f.fail = 1;
    io = io_of(&f, "mp4");
    v = reelcore_open_io(&io, NULL, REELCORE_ASYNC | REELCORE_NO_AUDIO);
    CHECK(v != NULL, "5: can't start");
    if (!v)
        return;
    r = wait_open(v);
    printf("  5. a read that fails: %s (%s)\n", r == REELCORE_FAILED ? "FAILED" : "?", reelcore_last_error());
    CHECK(r == REELCORE_FAILED, "5: gave %d, not FAILED", r);
    reelcore_close(v);
    feed_free(&f);
    CHECK(reelcore_open_io(NULL, NULL, 0) == NULL, "5: opened nothing");
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: io_test SRC DASHDIR\n");
        return 2;
    }
    arriving(argv[2]);
    later_segment(argv[1], argv[2]);
    seekable(argv[1]);
    close_waiting(argv[2]);
    failing(argv[2]);
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
