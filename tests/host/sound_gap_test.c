/*
 * reelcore with the sound from a second input (TubeSearch's YouTube video:
 * the picture and the sound as two HLS playlists) when the sound stops
 * arriving for a while, as when the network buffers, played through (fake)
 * SharedSoundBuffer as on RISC OS.
 *
 * Before: StreamManager ran dry, but a bit of sound short of a whole block
 * was still waiting in reelcore, so the "sound device isn't playing" check
 * saw sound queued and the clock not moving, and after a second turned the
 * sound off for good; the picture went on. Now that check looks only at
 * what StreamManager itself holds, and a stream that still won't play when
 * it has sound is started again (a few times a minute) before giving up.
 *
 *   sound_gap_test VIDEO SOUND SECONDS
 *   (VIDEO: a file with only a picture; SOUND: one with only sound, Opus,
 *    whose 20 ms frames don't fill StreamManager's blocks exactly;
 *    SECONDS: how long they are)
 * Each case: the sound input gives nothing (EAGAIN, as the network reader
 * does) from 3 to 6 s of the test's clock.
 *   1. StreamManager plays again once it has sound (as it should);
 *   2. StreamManager stops for good when it runs dry, so the stream must be
 *      started again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include "kernel.h"
#include "reelcore.h"
#include "fake_sdl_gl.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ---- the sound input going quiet ---- */
static double gap_from = 3, gap_to = 6;
static int gaps;
int __real_av_read_frame(AVFormatContext *s, AVPacket *pkt);
int __wrap_av_read_frame(AVFormatContext *s, AVPacket *pkt)
{
    if (s->nb_streams == 1 && s->streams[0]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
        fake_time >= gap_from && fake_time < gap_to) {
        gaps++;
        return AVERROR(EAGAIN);
    }
    return __real_av_read_frame(s, pkt);
}

/* ---- SharedSoundBuffer + StreamManager: plays what was added at the
   stream's rate, going by fake_time; stop_when_dry: having run out, it
   plays nothing more until a new stream is opened ---- */
static int ssb_open, ssb_opens, ssb_playing, ssb_rate, stop_when_dry, ssb_dead;
static unsigned ssb_added, ssb_played, ssb_limit;
static double ssb_last, played_total;

static void ssb_advance(void)
{
    if (ssb_open && ssb_playing && !ssb_dead && fake_time > ssb_last) {
        unsigned n = (unsigned)((fake_time - ssb_last) * ssb_rate * 4) & ~3u;
        if (n >= ssb_added - ssb_played) {
            n = ssb_added - ssb_played;
            if (stop_when_dry && ssb_added)
                ssb_dead = 1;
        }
        ssb_played += n;
        played_total += n / (ssb_rate * 4.0);
    }
    ssb_last = fake_time;
}

_kernel_oserror *_kernel_swi(int swi, _kernel_swi_regs *in, _kernel_swi_regs *out)
{
    static _kernel_oserror full = { 2, "full" }, unknown = { 3, "No such SWI" }, err = { 1, "fake" };
    ssb_advance();
    switch (swi) {
    case 0x39:                                                    /* OS_SWINumberFromString */
        return strncmp((char *)(intptr_t)in->r[1], "SharedSoundBuffer_", 18) &&
               strncmp((char *)(intptr_t)in->r[1], "StreamManager_", 14) ? &unknown : NULL;
    case 0x55FC0: ssb_open = 1; ssb_opens++; ssb_playing = 1; ssb_dead = 0; ssb_added = ssb_played = 0;
                  out->r[0] = 7; return NULL;                     /* OpenStream */
    case 0x55FCE: out->r[0] = 9; return NULL;                     /* ReturnStreamHandle */
    case 0x57287: ssb_limit = in->r[1]; return NULL;              /* SetBuffer */
    case 0x55FC5: ssb_rate = in->r[1] / 1024; return NULL;        /* SampleRate */
    case 0x55FC4: return NULL;                                    /* Volume */
    case 0x55FC9: ssb_playing = in->r[1] & 1; return NULL;        /* Pause */
    case 0x55FC1: ssb_open = 0; return NULL;                      /* CloseStream */
    case 0x57282:                                                 /* AddBlock */
        if (ssb_added - ssb_played + in->r[2] > ssb_limit)
            return &full;
        ssb_added += in->r[2];
        return NULL;
    case 0x57288: out->r[0] = ssb_added; out->r[1] = ssb_played; return NULL;   /* BufferStats */
    }
    return &err;
}

/* libavformat's AcornSSL backend refers to UnixLib's __get_ro_socket */
__attribute__((weak)) int __get_ro_socket(int fd) { return fd; }

static void play(const char *video, const char *sound, double seconds, int dry_stops, const char *what)
{
    ReelCoreSource src = { video, sound, NULL, NULL, NULL };
    ReelCore *v;
    ReelCoreStats st;
    int r = 0, opens0 = ssb_opens;
    double t0 = fake_time;
    stop_when_dry = dry_stops;
    played_total = 0;
    gaps = 0;
    gap_from = t0 + 3;
    gap_to = t0 + 6;
    v = reelcore_open_source(&src, 0);
    CHECK(v != NULL, "%s: can't open %s + %s", what, video, sound);
    if (!v)
        return;
    for (int i = 0; i < 20000 && r != REELCORE_END; i++) {
        r = reelcore_update(v);
        fake_time += 0.005;
    }
    reelcore_stats(v, &st);
    printf("  %s: %.2f s of sound played of %.2f, %d streams, sound %s, %d reads without sound, end %d, %.1f s\n",
           what, played_total, seconds, ssb_opens - opens0, st.sound_stalled ? "turned off" : "on", gaps,
           r == REELCORE_END, fake_time - t0);
    CHECK(gaps > 10, "%s: the sound input never went quiet", what);
    CHECK(!st.sound_stalled && reelcore_has_audio(v), "%s: the sound was turned off after the gap", what);
    CHECK(played_total > seconds - (dry_stops ? 1.5 : 0.3), "%s: only %.2f s of %.2f s of sound played",
          what, played_total, seconds);
    CHECK(r == REELCORE_END, "%s: didn't reach the end", what);
    CHECK(dry_stops ? ssb_opens - opens0 >= 2 : ssb_opens - opens0 == 1,
          "%s: %d sound streams (want %s)", what, ssb_opens - opens0, dry_stops ? "2 or more" : "1");
    reelcore_close(v);
}

int main(int argc, char **argv)
{
    double seconds;
    if (argc != 4) {
        fprintf(stderr, "usage: sound_gap_test VIDEO SOUND SECONDS\n");
        return 2;
    }
    seconds = atof(argv[3]);
    play(argv[1], argv[2], seconds, 0, "StreamManager plays again");
    play(argv[1], argv[2], seconds, 1, "StreamManager stops once dry");
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return !!fails;
}
