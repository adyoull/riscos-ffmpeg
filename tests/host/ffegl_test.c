/*
 * Host test of ffegl (ffegl/ffegl.c) with fake EGL (fake_riscos.c), fake
 * SDL audio and GL and a fake clock (fake_sdl_gl.c).
 *
 *   ffegl_test CLIP_WITH_SOUND   (1 s, 25 fps, e.g. tests/qemu/samples/h264_aac_640_360.mp4)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "ffegl.h"
#include "fake_riscos.h"
#include "fake_sdl_gl.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* Plays to the end in 10 ms steps. Checks each new frame is shown on time:
 * not before its time, and at most one step + one frame late. */
static int play(FFEGLVideo *v, double max_s, double *first_pts, double *last_pts, int *late)
{
    int frames = 0, r;
    double start = fake_time, prev = -1, t_first = 0, ref_pts = 0;
    *late = 0;
    while ((r = ffegl_update(v)) != FFEGL_END && fake_time - start < max_s) {
        CHECK(r >= 0, "update error %d", r);
        if (r == FFEGL_NEW_FRAME) {
            double p = ffegl_position(v), t = fake_time - start;
            /* The very first picture is shown at once; the timing is
               measured from the second (with sound, the clock also allows
               for the device's buffer, so the second can come a little
               later than its distance from the first). */
            if (!frames) *first_pts = p;
            if (frames == 1) { ref_pts = p; t_first = t; }
            if (frames) {
                CHECK(p > prev, "frame %d: pts %.3f after %.3f", frames, p, prev);
                /* each frame shown at its time relative to the first one:
                   not early, and no more than one 10 ms step late */
                double due = t_first + (p - ref_pts);
                if (t < due - 0.011 || t > due + 0.011)
                    (*late)++;
            }
            prev = p;
            frames++;
        }
        fake_time += 0.01;
    }
    *last_pts = prev;
    return r == FFEGL_END ? frames : -frames;
}

static void check_pixels(FFEGLVideo *v, const char *clip, int bgr)
{
    /* the same picture converted separately: decode the clip's last frame */
    int w = 800, h = 600, pitch = w * 4, bad = 0, border = 0, rw, rh, x, y;
    uint8_t *buf = malloc(pitch * h), *want;
    AVFormatContext *ic = NULL;
    AVCodecContext *dc;
    const AVCodec *codec;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc(), *last = av_frame_alloc();
    struct SwsContext *c;
    int vs;

    memset(buf, 0x55, pitch * h);
    CHECK(ffegl_draw_pixels(v, buf, pitch, w, h, bgr, 0) == 0, "draw_pixels failed");
    avformat_open_input(&ic, clip, NULL, NULL);
    avformat_find_stream_info(ic, NULL);
    vs = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    dc = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dc, ic->streams[vs]->codecpar);
    avcodec_open2(dc, codec, NULL);
    for (;;) {
        int eof = av_read_frame(ic, pkt) < 0;
        if (!eof && pkt->stream_index != vs) { av_packet_unref(pkt); continue; }
        avcodec_send_packet(dc, eof ? NULL : pkt);
        av_packet_unref(pkt);
        while (avcodec_receive_frame(dc, f) >= 0) { av_frame_unref(last); av_frame_move_ref(last, f); }
        if (eof) break;
    }
    rh = h; rw = (int)av_rescale(h, last->width, last->height);
    if (rw > w) { rw = w; rh = (int)av_rescale(w, last->height, last->width); }
    x = (w - rw) / 2; y = (h - rh) / 2;
    want = malloc(rw * rh * 4);
    c = sws_getContext(last->width, last->height, last->format, rw, rh,
                       bgr ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    {
        uint8_t *d[4] = { want }; int ds[4] = { rw * 4 };
        sws_scale(c, (const uint8_t * const *)last->data, last->linesize, 0, last->height, d, ds);
    }
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint8_t *p = buf + j * pitch + i * 4;
            if (i >= x && i < x + rw && j >= y && j < y + rh) {
                if (memcmp(p, want + ((j - y) * rw + i - x) * 4, 3)) bad++;
            } else if (p[0] | p[1] | p[2]) border++;
        }
    CHECK(!bad && !border, "draw_pixels(bgr %d): %d wrong, %d border pixels", bgr, bad, border);
    printf("  draw_pixels bgr=%d: %dx%d picture at %d,%d in 800x600\n", bgr, rw, rh, x, y);

    /* the texture holds the frame at its own size, RGBA */
    {
        unsigned int tex = 0;
        uint8_t *full = malloc(last->width * last->height * 4);
        uint8_t *d[4] = { full }; int ds[4] = { last->width * 4 };
        struct SwsContext *c2 = sws_getContext(last->width, last->height, last->format, last->width,
                                               last->height, AV_PIX_FMT_RGBA, SWS_POINT, NULL, NULL, NULL);
        sws_scale(c2, (const uint8_t * const *)last->data, last->linesize, 0, last->height, d, ds);
        { int im0 = fake_tex_images; int tr = ffegl_texture(v, 0, &tex);
          CHECK(fake_tex_images == im0 + 1, "texture create: no glTexImage2D"); CHECK(tr == 0 && tex == 42, "texture create: %d tex %u", tr, tex); }
        CHECK(fake_tex_w == last->width && fake_tex_h == last->height, "texture %dx%d", fake_tex_w, fake_tex_h);
        CHECK(fake_tex && !memcmp(fake_tex, full, last->width * last->height * 4), "texture pixels differ");
        {
            int im = fake_tex_images, sub = fake_tex_subimages;
            CHECK(ffegl_texture(v, tex, NULL) == 0 && fake_tex_subimages == sub + 1 && fake_tex_images == im,
                  "texture update: %d images %d subimages", fake_tex_images - im, fake_tex_subimages - sub);
        }
        sws_freeContext(c2);
        free(full);
    }
    sws_freeContext(c);
    free(want); free(buf);
    av_frame_free(&f); av_frame_free(&last); av_packet_free(&pkt);
    avcodec_free_context(&dc); avformat_close_input(&ic);
}

/* Waits (fake time) for the next new frame. */
static int next_frame(FFEGLVideo *v)
{
    for (int i = 0; i < 100; i++) {
        int r = ffegl_update(v);
        fake_time += 0.01;
        if (r == FFEGL_NEW_FRAME) return 1;
        if (r != FFEGL_SAME_FRAME) return 0;
    }
    return 0;
}

/* The sprite's pixels (R,G,B or B,G,R, top byte unused) equal the frame
   drawn by ffegl_draw_pixels at its own size (checked against an
   independent conversion in check_pixels). Returns the wrong pixels. */
static int image_differs(FFEGLVideo *v)
{
    int w = fake_img_w, h = fake_img_h, bad = 0;
    uint8_t *want = malloc((size_t)w * h * 4);
    ffegl_draw_pixels(v, want, w * 4, w, h, fake_img_bgr, FFEGL_STRETCH);
    for (int i = 0; i < w * h; i++)
        bad += memcmp(fake_img_pixels + i * 4, want + i * 4, 3) != 0;
    free(want);
    return bad;
}

/* ffegl_texture through EGLImage: the texture uses a sprite that each
   frame is converted into, with no GL upload; clean-up order; fallbacks. */
static void check_texture_image(const char *clip)
{
    for (int visual = 0; visual <= 0x4000; visual += 0x4000) {
        unsigned int tex = 0;
        int im, sub, binds, bad;
        uint32_t sum0 = 0, sum1 = 0;
        FFEGLVideo *v;
        fake_gl_eglimage = 1; fake_visual = visual;
        v = ffegl_open(clip, FFEGL_NO_AUDIO);
        CHECK(v && next_frame(v), "image: no frame");
        im = fake_tex_images; sub = fake_tex_subimages;
        CHECK(ffegl_texture(v, 0, &tex) == 0 && tex == 42, "image: texture failed");
        CHECK(fake_images == 1 && fake_img_binds > 0 && fake_tex_linked == 42, "image: not bound (%d images)", fake_images);
        CHECK(fake_img_bgr == (visual != 0), "image: colour order %d for visual 0x%x", fake_img_bgr, visual);
        CHECK(fake_img_w == ffegl_width(v) && fake_img_h == ffegl_height(v), "image: %dx%d", fake_img_w, fake_img_h);
        CHECK(((uintptr_t)fake_img_pixels & 15) == 0, "image: pixels not 16-byte aligned");
        CHECK((bad = image_differs(v)) == 0, "image: %d wrong pixels", bad);
        for (int i = 0; i < fake_img_w * fake_img_h * 4; i += 97) sum0 += fake_img_pixels[i];

        binds = fake_img_binds;
        for (int n = 0; n < 5; n++) {
            CHECK(next_frame(v), "image: no next frame");
            CHECK(ffegl_texture(v, tex, NULL) == 0, "image: update failed");
        }
        for (int i = 0; i < fake_img_w * fake_img_h * 4; i += 97) sum1 += fake_img_pixels[i];
        CHECK(fake_img_binds == binds && fake_images == 1, "image: rebound on update");
        CHECK(fake_tex_images == im && fake_tex_subimages == sub, "image: %d glTexImage2D, %d glTexSubImage2D",
              fake_tex_images - im, fake_tex_subimages - sub);
        CHECK(sum1 != sum0 && (bad = image_differs(v)) == 0, "image update: %d wrong pixels", bad);
        printf("  EGLImage visual 0x%04x: %dx%d sprite, updated in place with no copies\n",
               visual, fake_img_w, fake_img_h);
        ffegl_close(v);
        CHECK(fake_images == 0 && fake_tex_linked == 0 && !fake_destroyed_linked,
              "image close: %d images, linked %u, destroyed while linked %d",
              fake_images, fake_tex_linked, fake_destroyed_linked);
    }

    /* the image can't be made: copies instead */
    {
        unsigned int tex = 0;
        int im = fake_tex_images, sub = fake_tex_subimages;
        FFEGLVideo *v = ffegl_open(clip, FFEGL_NO_AUDIO);
        fake_img_fail = 1;
        next_frame(v);
        CHECK(ffegl_texture(v, 0, &tex) == 0 && fake_tex_images == im + 1 && !fake_images,
              "image failed: no fallback");
        next_frame(v);
        CHECK(ffegl_texture(v, tex, NULL) == 0 && fake_tex_subimages == sub + 1, "image failed: no update");
        ffegl_close(v);
        fake_img_fail = 0;
    }
    /* FFEGL_NO_EGLIMAGE=1 */
    {
        unsigned int tex = 0;
        int im = fake_tex_images;
        FFEGLVideo *v = ffegl_open(clip, FFEGL_NO_AUDIO);
        setenv("FFEGL_NO_EGLIMAGE", "1", 1);
        next_frame(v);
        CHECK(ffegl_texture(v, 0, &tex) == 0 && fake_tex_images == im + 1 && !fake_images,
              "FFEGL_NO_EGLIMAGE ignored");
        ffegl_close(v);
        unsetenv("FFEGL_NO_EGLIMAGE");
    }
    fake_gl_eglimage = 0; fake_visual = 0;
    printf("  EGLImage fallbacks: copies when the image fails or FFEGL_NO_EGLIMAGE=1\n");
}

int main(int argc, char **argv)
{
    const char *clip = argv[1];
    setvbuf(stdout, NULL, _IONBF, 0);
    FFEGLVideo *v;
    double first, last;
    int n, late, r;

    /* 1. with sound: the pictures follow the sound device */
    v = ffegl_open(clip, 0);
    CHECK(v && ffegl_has_audio(v) && fake_audio_open && !fake_audio_paused, "open with sound");
    printf("%dx%d, %.2f fps, %.2f s\n", ffegl_width(v), ffegl_height(v), ffegl_frame_rate(v), ffegl_duration(v));
    n = play(v, 5, &first, &last, &late);
    printf("  sound clock: %d frames, %.3f..%.3f, %d off time, ended at %.2f s\n", n, first, last, late, fake_time);
    CHECK(n == 25 && late == 0, "sound clock: %d frames, %d off time", n, late);
    CHECK(fake_time > 0.95 && fake_time < 1.3, "sound clock: ended at %.2f s", fake_time);
    check_pixels(v, clip, 0);
    check_pixels(v, clip, 1);

    /* 2. draw into an EGL surface */
    {
        int dummy_w = 640;
        (void)dummy_w;
        fake_desktop = 1; fake_visual = 0x4000;
        fake_win_w = 700; fake_win_h = 500;
        EGLSurface s = eglCreateWindowSurface(eglGetDisplay(0), (EGLConfig)1, (EGLNativeWindowType)0x5000, NULL);
        CHECK(ffegl_draw_surface(v, eglGetDisplay(0), s, 0, 0, 0, 0, 0) == 0 && !fake_locked, "draw_surface");
        /* 0x00RRGGBB: blue in the first byte of each pixel, red in the third */
        {
            uint8_t px[4];
            uint8_t *mem = NULL;
            EGLAttribKHR p;
            eglLockSurfaceKHR(0, s, NULL);
            eglQuerySurface64KHR(0, s, EGL_BITMAP_POINTER_KHR, &p);
            mem = (uint8_t *)p;
            memcpy(px, mem, 4);               /* top left: a black bar */
            eglUnlockSurfaceKHR(0, s);
            CHECK(!px[0] && !px[1] && !px[2], "draw_surface: bar not black");
        }
        eglDestroySurface(0, s);
    }

    /* 3. pause: the position stays */
    ffegl_seek(v, 0);
    r = ffegl_update(v);
    CHECK(r == FFEGL_NEW_FRAME && ffegl_position(v) < 0.001, "seek to 0: %d %.3f", r, ffegl_position(v));
    for (int i = 0; i < 20; i++) { ffegl_update(v); fake_time += 0.01; }
    ffegl_pause(v, 1);
    {
        double p0 = ffegl_position(v);
        CHECK(fake_audio_paused, "pause: sound not paused");
        for (int i = 0; i < 50; i++) { CHECK(ffegl_update(v) != FFEGL_NEW_FRAME, "pause: new frame"); fake_time += 0.01; }
        CHECK(ffegl_position(v) == p0, "pause: moved");
        ffegl_pause(v, 0);
        CHECK(!fake_audio_paused, "resume: sound still paused");
        n = play(v, 5, &first, &last, &late);
        CHECK(n > 0 && late == 0 && fabs(first - p0) < 0.05, "after pause: %d frames from %.3f, %d off time", n, first, late);
    }

    /* 4. seek to the middle */
    ffegl_seek(v, 0.5);
    r = ffegl_update(v);
    CHECK(r == FFEGL_NEW_FRAME && ffegl_position(v) >= 0.5 - 0.001 && ffegl_position(v) < 0.55,
          "seek 0.5: %d at %.3f", r, ffegl_position(v));
    n = play(v, 5, &first, &last, &late);
    CHECK(n > 5 && late == 0, "after seek: %d frames, %d off time", n, late);
    ffegl_close(v);
    CHECK(!fake_audio_open, "close: sound device left open");

    /* 5. no sound: the timer */
    v = ffegl_open(clip, FFEGL_NO_AUDIO);
    CHECK(v && !ffegl_has_audio(v) && !fake_audio_open, "open without sound");
    n = play(v, 5, &first, &last, &late);
    printf("  timer clock: %d frames, %d off time\n", n, late);
    CHECK(n == 25 && late == 0, "timer: %d frames, %d off time", n, late);
    ffegl_close(v);

    /* 6. sound device refuses: falls back to the timer */
    fake_audio_fail = 1;
    v = ffegl_open(clip, 0);
    CHECK(v && !ffegl_has_audio(v), "no sound device");
    n = play(v, 5, &first, &last, &late);
    CHECK(n == 25 && late == 0, "no device: %d frames, %d off time", n, late);
    ffegl_close(v);
    fake_audio_fail = 0;

    /* 6b. the sound device opens but never plays (no SharedSoundBuffer):
       after a second the pictures carry on with the timer */
    fake_audio_stall = 1;
    v = ffegl_open(clip, 0);
    {
        double t0 = fake_time;
        char info[256];
        n = play(v, 8, &first, &last, &late);
        ffegl_info(v, info, sizeof(info));
        printf("  stalled sound device: %d frames, ended after %.2f s; %s\n", n, fake_time - t0, info);
        CHECK(n >= 20 && fake_time - t0 < 2.6 && !ffegl_has_audio(v) && strstr(info, "isn't playing"),
              "stalled sound: %d frames in %.2f s, has_audio %d", n, fake_time - t0, ffegl_has_audio(v));
    }
    ffegl_close(v);
    fake_audio_stall = 0;

    /* 7. loop: goes round again */
    v = ffegl_open(clip, FFEGL_LOOP);
    {
        int frames = 0, wraps = 0;
        double prev = -1;
        for (int i = 0; i < 260; i++) {        /* 2.6 s */
            r = ffegl_update(v);
            CHECK(r != FFEGL_END, "loop: ended");
            if (r == FFEGL_NEW_FRAME) {
                if (ffegl_position(v) < prev) wraps++;
                prev = ffegl_position(v);
                frames++;
            }
            fake_time += 0.01;
        }
        printf("  loop: %d frames in 2.6 s, %d restarts\n", frames, wraps);
        CHECK(wraps == 2 && frames >= 60, "loop: %d frames, %d restarts", frames, wraps);
    }
    ffegl_close(v);

    /* 8. textures through EGLImage */
    check_texture_image(clip);

    /* 9. a file that isn't there */
    CHECK(!ffegl_open("/nonexistent/x.mp4", 0) && *ffegl_last_error(), "missing file");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
