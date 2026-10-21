/*
 * prof_reel: plays a file through reelcore as Reel does, for an instruction
 * profile (tools/profile/run.sh runs it under qemu-arm with insprof).
 * Every picture is decoded and drawn (the clock is the tests' fake one: it
 * only moves between updates, so nothing is ever late or skipped).
 *
 *   prof_reel FILE MODE [W H] [options]
 *     MODE overlay  each picture into YV12 planes W x H (default the
 *                   picture's size; halved when it's over 1920x1088), as
 *                   Reel's VideoOverlay path
 *     MODE sprite   each picture into 32bpp W x H (default 1440x810,
 *                   scaled to fit), as Reel's sprite path (no overlay)
 *     -panel        the stats panel shown
 *     -subs FILE    a subtitle file shown
 *     -sound        the sound decoded too (to the tests' fake output)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "reelcore.h"
#include "fake_sdl_gl.h"

int main(int argc, char **argv)
{
    const char *file, *mode, *subs = NULL;
    int w = 0, h = 0, panel = 0, sound = 0, pictures = 0, r = 0;
    ReelCore *v;
    if (argc < 3) {
        fprintf(stderr, "usage: prof_reel FILE overlay|sprite [W H] [-panel] [-subs FILE] [-sound]\n");
        return 2;
    }
    file = argv[1];
    mode = argv[2];
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-panel"))
            panel = 1;
        else if (!strcmp(argv[i], "-sound"))
            sound = 1;
        else if (!strcmp(argv[i], "-subs") && i + 1 < argc)
            subs = argv[++i];
        else if (!w && i + 1 < argc) {
            w = atoi(argv[i]);
            h = atoi(argv[++i]);
        }
    }
    setenv("FAKE_AUDIO_BLOCK", "2048", 1);
    v = reelcore_open(file, (sound ? 0 : REELCORE_NO_AUDIO) | REELCORE_NO_VIDEOCORE | REELCORE_NO_HEVC_BLOCK);
    if (!v) {
        fprintf(stderr, "can't open %s: %s\n", file, reelcore_last_error());
        return 1;
    }
    if (subs && reelcore_add_subtitle_file(v, subs) < 0)
        fprintf(stderr, "can't read %s\n", subs);
    if (panel) {
        static float g[60];
        ReelCorePanel p;
        memset(&p, 0, sizeof p);
        for (int i = 0; i < 60; i++)
            g[i] = (float)(i % 7) / 7;
        p.rows = 4;
        p.label[0] = "Decoder"; p.value[0] = "h264 (ARM)";
        p.label[1] = "Decode"; p.value[1] = "12.3 ms"; p.graph[1] = g; p.graph_rgb[1] = 0x40C040;
        p.label[2] = "Shown"; p.value[2] = "25 a second";
        p.label[3] = "Dropped"; p.value[3] = "0";
        p.graph_n = 60;
        p.yuv_scale = 1;
        reelcore_set_panel(v, &p);
    }
    if (!strcmp(mode, "sprite") && !w) {
        w = 1440;
        h = 810;
    }
    {
        uint8_t *buf = NULL, *planes[3];
        int pitch[3];
        for (int i = 0; i < 200000 && r != REELCORE_END; i++) {
            r = reelcore_update(v);
            if (r == REELCORE_NEW_FRAME && !buf) {   /* (the size is known from the first picture) */
                if (!w) {
                    reelcore_frame_size(v, &w, &h);
                    if (w > 1920 || h > 1088) {     /* (Reel halves 4K into an HD overlay) */
                        w /= 2;
                        h /= 2;
                    }
                    w &= ~1;
                    h &= ~1;
                }
                size_t ysz = (size_t)(w + 64) * (h + 2);
                buf = malloc(ysz * 4 + 64);
                planes[0] = buf;
                planes[1] = buf + ysz;
                planes[2] = buf + ysz + ysz / 4 + 32;
                pitch[0] = w;
                pitch[1] = pitch[2] = w / 2;
            }
            if (r == REELCORE_NEW_FRAME) {
                pictures++;
                if (!strcmp(mode, "overlay"))
                    reelcore_draw_yuv420(v, planes, pitch, w, h, NULL);
                else
                    reelcore_draw_pixels(v, buf, w * 4, w, h, 0, 0);
            }
            fake_time += 0.002;
        }
        free(buf);
    }
    printf("%s: %d pictures drawn (%s %dx%d%s%s%s)\n", file, pictures, mode, w, h, panel ? ", panel" : "",
           subs ? ", subtitles" : "", sound ? ", sound" : "");
    reelcore_close(v);
    return 0;
}
